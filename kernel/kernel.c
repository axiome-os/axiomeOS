#include "serial.h"
#include "printk.h"
#include "axboot.h"
#include "framebuffer.h"
#include "pmm.h"
#include "apic.h"
#include "clock.h"
#include "cmos.h"
#include "rtc_efi.h"
#include "rtc_sync.h"
#include "tss.h"
#include "ioapic.h"
#include "keyboard.h"
#include "tty.h"
#include "vfs.h"
#include "pci.h"
#include "driver.h"
#include "mouse.h"
#include "softirq.h"
#include "vmm.h"
#include "slab.h"
#include "sched.h"
#include "syscall.h"
#include "elf.h"
#include "ide.h"
#include "fat32.h"
#include "axiomefs.h"
#include "security.h"
#include "netdev.h"
#include "socket.h"
#include "loopback.h"
#include "tcp.h"
#include "arp.h"
#include "module.h"
#include "acpi.h"
#include "mmap.h"
#include "hal/cshim.h"
#include "hal/hal_bootinfo.h"
#include "gfx/manager.h"
#include "xhci.h"

void axboot_parse(struct axboot_info *info);
void isr_init(void);

/* Load the init program from the root partition and start it as PID 1.
   The buffer is freed after exec_user_program() copies it into the new
   address space. */
static void exec_init_from_disk(void)
{
    uint8_t *buf = 0;
    size_t size = 0;
    if (vfs_read_file("/bin/init", &buf, &size) != 0)
    {
        printk("Init: cannot load /bin/init from disk\n");
        return;
    }
    printk("Init: loaded %lu bytes from /bin/init\n", size);
    exec_user_program(buf, size, "init");
    kfree(buf);
}

/* Respawn the userspace init process after it has exited. */
void kernel_respawn_init(void)
{
    printk("Init: respawning userspace init\n");
    exec_init_from_disk();
}

void kmain(struct axboot_info *info)
{
    hal_init();
    serial_init(COM1);

    printk("axiomeOS booting...\n");

    axboot_parse(info);

    pmm_init();
    vmm_init();
    fb_init_buffers();
    acpi_init(acpi_rsdp_addr);

    /* Boot environment as seen through the HAL (filled by the boot glue,
       e.g. mb2.c). */
    struct hal_bootinfo *bi = hal_bootinfo();

    if (bi->framebuffer_present)
    {
        fb_clear();
        fb_write("Hello, axiomeOS!\n");
        fb_write("Framebuffer active\n");
    }
    else
    {
        printk("No framebuffer available (use -vga std)\n");
    }

    void *p = pmm_alloc_frame();
    printk("PMM: allocated frame at 0x%lx\n", (unsigned long)p);
    if (p)
    {
        pmm_free_frame(p);
        printk("PMM: freed\n");
    }

    slab_init();

    printk("--- Phase 4: Memory stress test ---\n");
    {
        uint64_t before = pmm_free_count();
        void *ptrs[32];
        int n;
        for (n = 0; n < 32; n++)
        {
            ptrs[n] = kmalloc(64);
            if (!ptrs[n]) break;
            *(volatile uint64_t *)ptrs[n] = 0xCAFEBABE;
        }
        printk("Slab: allocated %d objects (64 bytes each)\n", n);

        for (int i = 0; i < n; i++)
        {
            if (*(volatile uint64_t *)ptrs[i] != 0xCAFEBABE)
                printk("  ERROR: data corruption at ptr[%d]\n", i);
            kfree(ptrs[i]);
        }
        uint64_t after = pmm_free_count();
        printk("Slab: freed all, frames before=%lu after=%lu\n", before, after);

        uint64_t phys = (uint64_t)pmm_alloc_frame();
        if (phys)
        {
            void *mapped = vmm_mmap_phys(phys, 1, MMU_WRITE);
            printk("VMM: mapped phys 0x%lx -> virt %p\n", phys, mapped);
            if (mapped)
            {
                *(volatile uint64_t *)mapped = 0xDEADBEEF;
                printk("VMM: wrote 0x%lx to mapped region, readback=0x%lx\n",
                       0xDEADBEEFull, *(volatile uint64_t *)mapped);
            }
            pmm_free_frame((void *)phys);
        }

        uint64_t demand_virt = 0xFFFFFE8000000000ULL;
        uint64_t dp_page = (uint64_t)pmm_alloc_frame();
        if (dp_page)
        {
            if (vmm_map_page(demand_virt, dp_page, MMU_WRITE) == 0)
            {
                *(volatile char *)demand_virt = 'D';
                uint64_t mapped = vmm_virt_to_phys(demand_virt);
                printk("VMM: demand-mapped 0x%lx -> phys 0x%lx (readback '%c')\n",
                       demand_virt, mapped, *(volatile char *)demand_virt);
            }
            else
                printk("VMM: demand map failed\n");
        }
    }
    printk("--- Phase 4: memory test done ---\n");

    tss_init();
    isr_init();
    /* Map EFI runtime services pages and read GetTime() now that the IDT is
       live and vmm_mmap_phys() is available.  Must be before hal_timer_start()
       changes any APIC state, and well before IRQs are enabled. */
    rtc_efi_init();
    hal_timer_start(0x10000);
    /* Start the periodic timer at the calibrated 1 ms period (count=0). */
    apic_timer_start(0);
    ioapic_init();
    keyboard_init();
    tty_init();
    serial_init_input();
    mouse_init();

    sched_init();

    syscall_init();

    vfs_init();

    procfs_init();

    pci_init();

    /* Graphics stack: GOP is already live via fb_*; now register every
       display backend and let the manager pick the best one. Intel/Bochs
       are probe-only stubs today, so this logs what PCI saw and keeps GOP
       as the scanout owner until native modeset lands. */
    gfx_init();
    printk("gfx: active='%s'\n", gfx_active_name());

    /* Network stack init: mbuf pool, loopback, protocol handlers.
       Must come before driver_init() because e1000 probe needs mbufs. */
    net_init();
    socket_init();

    driver_init();
    devfs_init();
    module_init_subsys();

    fat32_automount();

    /* Mount the axiomefs persistent root from partition 1 of the boot disk
       (second MBR partition, 0-indexed) and parse /etc/passwd into the
       in-kernel user database. Try IDE first, then USB MSC (flash drive). */
    if (axiomefs_mount_part(0, 0, 1, "/") != 0) {
        for (int i = 0; i < 16; i++) {
            struct block_dev *bd = xhci_get_block_dev(i);
            if (!bd) break;
            if (axiomefs_mount_block(bd, 1, "/") == 0)
                break;
        }
    }
    security_init();

    klog_init_late();

    uint64_t sys_ret = syscall_dispatch(SYS_PRINT, (uint64_t)"hello from syscall dispatch", 0, 0, 0, 0);
    printk("Syscall dispatch returned: %lu\n", sys_ret);

    exec_init_from_disk();

    uint64_t yield_count = 0;
    printk("APIC: tick");
    hal_cpu_irq_enable();

    /* Post-IRQ calibration: count timer ISR firings over a PIT busy-wait.
       This must run after IRQs are enabled and the periodic timer is firing. */
    apic_calibrate_post_irq();

    /* Determine boot epoch:
       1. Try EFI runtime services GetTime() — precise UTC from firmware RTC.
       2. Fall back to BUILD_TIME_UNIX (compile-time timestamp) if EFI
          is unavailable or returns an error. */
    uint64_t boot_unix = rtc_efi_get_unix();
    if (boot_unix == 0)
    {
        boot_unix = BUILD_TIME_UNIX;
        printk("Clock: EFI time unavailable, using build timestamp\n");
    }
    else
    {
        printk("Clock: using EFI RTC time\n");
    }

    /* Read timezone offset from /System/Configuration/timezone.
       File format: a single signed integer, seconds east of UTC (e.g. "10800").
       Falls back to the build-host offset (BUILD_TZ_OFFSET_SEC) if absent. */
    int64_t tz_offset = (int64_t)BUILD_TZ_OFFSET_SEC;
    {
        uint8_t *tz_buf = 0;
        size_t   tz_len = 0;
        if (vfs_read_file("/System/Configuration/timezone", &tz_buf, &tz_len) == 0
            && tz_buf && tz_len > 0)
        {
            int64_t v = 0;
            int neg = 0;
            size_t i = 0;
            if (tz_buf[i] == '-') { neg = 1; i++; }
            else if (tz_buf[i] == '+') { i++; }
            for (; i < tz_len && tz_buf[i] >= '0' && tz_buf[i] <= '9'; i++)
                v = v * 10 + (tz_buf[i] - '0');
            tz_offset = neg ? -v : v;
            kfree(tz_buf);
            printk("Clock: timezone from config: %ld s\n", (long)tz_offset);
        }
        else
        {
            printk("Clock: timezone from build default: %ld s\n", (long)tz_offset);
        }
    }
    clock_init(boot_unix, tz_offset);

    /* Start CMOS RTC drift correction service: checks every 5 s, corrects
       if wall time is off by >= 2 s compared to the hardware RTC. */
    rtc_sync_init();

    klog_flush();

    /* Hand control to the shell with a clean framebuffer: the boot logs that
       scrolled above are not kernel noise the user needs to see. Scheduler
       bookkeeping is routed to klog() (serial + /var/log/kernel.log only), so the
       running shell stays free of visual noise. */
    if (fb_active())
        fb_clear();

    sched_yield();

    while (1)
    {
        softirq_poll();
        netdev_poll_all();
        xhci_poll();

        yield_count++;
        if ((yield_count % 50) == 0)
        {
            arp_tick();
            tcp_tick((uint32_t)(yield_count * 10));  /* rough ms estimate */
        }
        if ((yield_count % 5) == 0)
        {
            klog_flush();
            fb_flush();
            sched_yield();
        }

        hal_cpu_halt();
    }
}
