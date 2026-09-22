/* axboot handoff parsing: validates struct axboot_info from the UEFI
   bootloader and publishes it to the kernel (mmap, framebuffer, ACPI)
   plus the portable hal_bootinfo. Replaces the old multiboot2 glue. */

#include <stdint.h>

#include <stddef.h>
#include "axboot.h"
#include "printk.h"
#include "framebuffer.h"
#include "mmap.h"
#include "hal/hal_bootinfo.h"

struct mmap_info kernel_mmap;
uint64_t mmap_max_addr;
void *acpi_rsdp_addr;

static const char *mmap_type_name(uint32_t type)
{
    switch (type)
    {
        case 1: return "Available";
        case 2: return "Reserved";
        case 3: return "ACPI reclaimable";
        case 4: return "ACPI NVS";
        case 5: return "Bad memory";
        default: return "Unknown";
    }
}

void axboot_parse(struct axboot_info *info)
{
    if (!info)
    {
        printk("axboot: null bootinfo!\n");
        return;
    }

    if (info->magic != AXIOME_BOOT_MAGIC)
    {
        printk("axboot: bad magic 0x%lx (want 0x%lx)\n",
               (unsigned long)info->magic,
               (unsigned long)AXIOME_BOOT_MAGIC);
        return;
    }
    if (info->version != AXBOOT_VERSION)
    {
        printk("axboot: version %u (want %u)\n",
               info->version, AXBOOT_VERSION);
        return;
    }

    printk("axboot: v%u kernel phys=0x%lx size=0x%lx cmdline='%s'\n",
           info->version,
           (unsigned long)info->kernel_phys_load,
           (unsigned long)info->kernel_size,
           info->cmdline);
    /* Publish cmdline to hal_bootinfo for gfx property parsing etc. */
    {
        struct hal_bootinfo *bi_cmd = hal_bootinfo();
        size_t i = 0;
        for (; i < sizeof(bi_cmd->cmdline) - 1 && info->cmdline[i]; i++)
            bi_cmd->cmdline[i] = info->cmdline[i];
        bi_cmd->cmdline[i] = '\0';
    }

    /* Memory map. */
    kernel_mmap.count = 0;
    mmap_max_addr = 0;
    if (info->mmap && info->mmap_count > 0)
    {
        uint32_t n = info->mmap_count;
        if (n > MAX_MMAP_ENTRIES)
        {
            printk("axboot: truncating mmap %u -> %u\n",
                   n, MAX_MMAP_ENTRIES);
            n = MAX_MMAP_ENTRIES;
        }
        printk("axboot: MMAP (%u entries)\n", n);
        for (uint32_t i = 0; i < n; i++)
        {
            uint64_t base = info->mmap[i].base;
            uint64_t len = info->mmap[i].length;
            uint32_t type = info->mmap[i].type;
            printk("  base=0x%lx len=0x%lx type=%s\n",
                   base, len, mmap_type_name(type));
            kernel_mmap.entries[kernel_mmap.count].base = base;
            kernel_mmap.entries[kernel_mmap.count].length = len;
            kernel_mmap.entries[kernel_mmap.count].type = type;
            if (type == 1)
            {
                uint64_t end = base + len;
                if (end > mmap_max_addr)
                    mmap_max_addr = end;
            }
            kernel_mmap.count++;
        }
    }
    else
    {
        printk("axboot: no memory map!\n");
    }
    hal_bootinfo()->mmap_max_addr = mmap_max_addr;

    /* Framebuffer. */
    if (info->fb_addr != 0 && info->fb_width > 0 && info->fb_height > 0)
    {
        const char *pf = "?";
        if (info->fb_pixel_format == AXB_PF_RGB_8_8_8)
            pf = "RGB";
        else if (info->fb_pixel_format == AXB_PF_BGR_8_8_8)
            pf = "BGR";
        printk("axboot: FB addr=0x%lx %ux%u pitch=%u bpp=%u fmt=%s\n",
               (unsigned long)info->fb_addr,
               info->fb_width, info->fb_height,
               info->fb_pitch, info->fb_bpp, pf);

        struct hal_bootinfo *bi = hal_bootinfo();
        bi->framebuffer_present = 1;
        bi->fb_addr = (uintptr_t)info->fb_addr;
        bi->fb_width = info->fb_width;
        bi->fb_height = info->fb_height;
        bi->fb_pitch = info->fb_pitch;
        bi->fb_bpp = (uint8_t)info->fb_bpp;
        bi->fb_type = 1; /* direct colour */

        fb_init((uintptr_t)info->fb_addr, info->fb_width,
                info->fb_height, info->fb_pitch,
                (uint8_t)info->fb_bpp, 1);
    }
    else
    {
        printk("axboot: no framebuffer\n");
    }

    /* ACPI / SMBIOS. Only the addresses are recorded here; the tables
       live in high physical memory that is not identity-mapped until
       mmu_arch_init() runs, so they must not be dereferenced yet
       (acpi_init() parses them after vmm_init()). */
    if (info->acpi_rsdp_addr != 0)
    {
        printk("axboot: ACPI RSDP at 0x%lx\n",
               (unsigned long)info->acpi_rsdp_addr);
        acpi_rsdp_addr = (void *)(uintptr_t)info->acpi_rsdp_addr;
        hal_bootinfo()->acpi_rsdp = acpi_rsdp_addr;
    }
    else
    {
        printk("axboot: no ACPI RSDP\n");
    }
    if (info->smbios_addr != 0)
        printk("axboot: SMBIOS at 0x%lx\n",
               (unsigned long)info->smbios_addr);
}
