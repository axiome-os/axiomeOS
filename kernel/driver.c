#include "driver.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "ide.h"
#include "serial.h"
#include "framebuffer.h"
#include "vmm.h"
#include "sched.h"
#include "xhci.h"
#include "input.h"
#include <stddef.h>

static struct driver *g_drivers;
static struct device *g_devices;

static void dname(char *dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < 31; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* ---------------------------------------------------------------- *
 * Registry
 * ---------------------------------------------------------------- */
void driver_register(struct driver *d)
{
    d->next = g_drivers;
    g_drivers = d;
}

void driver_unregister(struct driver *d)
{
    if (d == g_drivers)
    {
        g_drivers = d->next;
        d->next = 0;
        return;
    }
    for (struct driver *p = g_drivers; p && p->next; p = p->next)
    {
        if (p->next == d)
        {
            p->next = d->next;
            d->next = 0;
            return;
        }
    }
}

void device_register(struct device *d)
{
    d->next = g_devices;
    g_devices = d;
}

struct device *device_find(const char *name)
{
    for (struct device *d = g_devices; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return 0;
}

int device_enumerate(struct device **out, int max)
{
    int n = 0;
    for (struct device *d = g_devices; d && n < max; d = d->next)
        out[n++] = d;
    return n;
}

/* ---------------------------------------------------------------- *
 * Probe matching
 * ---------------------------------------------------------------- */
int driver_probe_pci(struct pci_device *pdev)
{
    /* Already claimed by a driver. Probing again would re-run hardware
       initialisation (and re-print "bound"): every .kxt init and every
       rescan walks the whole PCI list, so this must be a silent no-op. */
    if (pdev->owner)
        return 1;
    for (struct driver *d = g_drivers; d; d = d->next)
    {
        int v_ok = (d->vendor == DRV_ANY) || (d->vendor == pdev->vendor);
        int d_ok = (d->device == DRV_ANY) || (d->device == pdev->device);
        int c_ok = (d->pci_class == (uint8_t)(DRV_ANY & 0xFF)) ||
                   (d->pci_class == pdev->class_code);
        int s_ok = (d->pci_subclass == (uint8_t)(DRV_ANY & 0xFF)) ||
                   (d->pci_subclass == pdev->subclass);
        if (v_ok && d_ok && c_ok && s_ok)
        {
            int result = d->probe ? d->probe(pdev) : 0;
            if (result < 0)
                continue;
            pdev->owner = d;
            printk("DRV: %s bound to %x:%x\n", d->name, (int)pdev->vendor, (int)pdev->device);
            return 1;
        }
    }
    return 0;
}

void driver_probe_all(void)
{
    for (struct pci_device *p = pci_first(); p; p = p->next)
        driver_probe_pci(p);
}

/* 12.9 Hotplug: re-enumerate the PCI bus and probe only what is new.
   pci_init() rebuilds the device list from scratch, so owners recorded on
   the old structs are re-attached by bus/dev/func first — otherwise every
   rescan would re-run all hardware init (and re-print every "bound" line).
   Functions that appear for the first time stay unclaimed and get probed. */
void driver_rescan(void)
{
    struct { uint8_t bus, dev, func; struct driver *owner; } keep[64];
    int n = 0;
    for (struct pci_device *p = pci_first(); p && n < 64; p = p->next)
    {
        if (!p->owner)
            continue;
        keep[n].bus = p->bus; keep[n].dev = p->dev;
        keep[n].func = p->func; keep[n].owner = p->owner;
        n++;
    }
    pci_init();
    for (struct pci_device *p = pci_first(); p; p = p->next)
        for (int i = 0; i < n; i++)
            if (p->bus == keep[i].bus && p->dev == keep[i].dev &&
                p->func == keep[i].func)
            {
                p->owner = keep[i].owner;
                break;
            }
    driver_probe_all();
}

/* ---------------------------------------------------------------- *
 * Device operations for built-in / working devices
 * ---------------------------------------------------------------- */
static long zero_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)off;
    memset(buf, 0, len);
    return (long)len;
}
static long zero_write(struct device *d, uint64_t off, const void *buf, size_t len)
{
    (void)d; (void)off; (void)buf;
    return (long)len;
}

/* ttyS0: the platform serial console through the HAL serial API. */
static long tty_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)off;
    size_t got = 0;
    while (got < len)
    {
        if (!serial_rx_ready(COM1))
            break;
        ((char *)buf)[got++] = serial_getc(COM1);
    }
    return (long)got;
}
static long tty_write(struct device *d, uint64_t off, const void *buf, size_t len)
{
    (void)d; (void)off;
    for (size_t i = 0; i < len; i++)
    {
        char c = ((const char *)buf)[i];
        if (c == '\n') serial_putchar(COM1, '\r');
        serial_putchar(COM1, c);
    }
    return (long)len;
}

/* Block device backed by the legacy ATA driver (ide.c). */
static long ideblk_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    struct block_dev *bd = (struct block_dev *)d->priv;
    if (!bd || !bd->present) return -1;
    uint64_t lba = off >> 9;
    uint32_t nsec = (uint32_t)((len + 511) >> 9);
    uint8_t *tmp = (uint8_t *)kmalloc(nsec * 512);
    if (!tmp) return -1;
    if (blk_read(bd, lba, nsec, tmp) != 0) { kfree(tmp); return -1; }
    size_t got = len; if (got > nsec * 512) got = nsec * 512;
    memcpy(buf, tmp, got);
    kfree(tmp);
    return (long)got;
}
static long ideblk_write(struct device *d, uint64_t off, const void *buf, size_t len)
{
    struct block_dev *bd = (struct block_dev *)d->priv;
    if (!bd || !bd->present) return -1;
    uint64_t lba = off >> 9;
    uint32_t nsec = (uint32_t)((len + 511) >> 9);
    uint8_t *tmp = (uint8_t *)kmalloc(nsec * 512);
    if (!tmp) return -1;
    memcpy(tmp, buf, len);
    if (blk_write(bd, lba, nsec, tmp) != 0) { kfree(tmp); return -1; }
    kfree(tmp);
    return (long)len;
}

/* ---------------------------------------------------------------- *
 * Built-in drivers
 * ---------------------------------------------------------------- */
static int ata_probe(struct pci_device *pdev)
{
    (void)pdev;
    if (device_find("ide0"))
        return 0;
    struct block_dev *bd = ide_get_dev(0, 0);
    if (!bd || !bd->present)
    {
        /* ide_probe may not have been called yet; trigger it. */
        static int tried;
        if (!tried) { ide_probe(0, 0); tried = 1; }
        bd = ide_get_dev(0, 0);
    }
    if (!bd || !bd->present)
        return 0;
    struct device *d = (struct device *)kmalloc(sizeof(struct device));
    memset(d, 0, sizeof(*d));
    dname(d->name, "ide0");
    d->major = 3; d->minor = 0; d->type = DEV_BLOCK;
    d->drv = 0; d->priv = bd;
    d->ops.read = ideblk_read;
    d->ops.write = ideblk_write;
    device_register(d);
    printk("DRV: ide0 -> /dev/ide0 (%lu sectors)\n", (unsigned long)bd->total_sectors);
    return 0;
}

/* ---- fb0 device (framebuffer) ---- */
static long fb_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d;
    if (!fb_active())
        return -1;
    /* Not meaningful for a framebuffer, but allow reading back pixels. */
    return -1;
}

static long fbdev_write(struct device *d, uint64_t off, const void *buf, size_t len)
{
    (void)d; (void)off;
    if (!fb_active())
        return -1;
    fb_write((const char *)buf);
    return (long)len;
}

static long fb_mmap(struct device *d, uint64_t off, uint64_t virt, size_t len, uint64_t flags)
{
    (void)d;
    (void)flags;
    if (!fb_active())
        return -1;
    struct thread *t = sched_current();
    if (!t || !t->mmu)
        return -1;
    size_t pages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (size_t i = 0; i < pages; i++)
    {
        uint64_t va = (uint64_t)(uintptr_t)fb_addr() + off + i * PAGE_SIZE;
        uint64_t pa = vmm_virt_to_phys(va);
        if (!pa)
            return -1;
        if (vmm_map_page_in(t->mmu, virt + i * PAGE_SIZE, pa,
                             MMU_USER | MMU_WRITE) < 0)
            return -1;
    }
    return 0;
}

static struct dev_ops fb_ops = {
    .read  = fb_read,
    .write = fbdev_write,
    .mmap  = fb_mmap,
};

static int vga_probe(struct pci_device *pdev)
{
    (void)pdev;
    if (device_find("fb0"))
        return 0;
    if (!fb_active())
        return 0;
    struct device *d = (struct device *)kmalloc(sizeof(struct device));
    memset(d, 0, sizeof(*d));
    dname(d->name, "fb0");
    d->major = 29; d->minor = 0; d->type = DEV_CHAR;
    d->ops = fb_ops;
    device_register(d);
    printk("DRV: fb0 -> /Devices/fb0 (framebuffer %ux%u)\n",
           fb_width(), fb_height());
    return 0;
}

/* NOTE: the NVMe and AHCI (SATA) drivers are built into the kernel image
   (modules/nvme.c and modules/sata.c compiled with AXIOME_BUILTIN_DRIVER,
   registered below). They used to ship as loadable .kxt modules; moving them
   in-kernel guarantees the boot disk drivers are present before init runs,
   with no dependence on /System/Extensions or the module loader. */

static struct driver ata_drv = {
    .name = "ata-ide", .vendor = DRV_ANY, .device = DRV_ANY,
    .pci_class = 0x01, .pci_subclass = 0x01, .probe = ata_probe,
};
static struct driver xhci_drv = {
    .name = "xhci", .vendor = DRV_ANY, .device = DRV_ANY,
    .pci_class = 0x0C, .pci_subclass = 0x03, .probe = xhci_probe,
};
static struct driver vga_drv = {
    .name = "vga", .vendor = 0x1234, .device = 0x1111,
    .pci_class = DRV_ANY, .pci_subclass = DRV_ANY, .probe = vga_probe,
};

/* NOTE: the e1000 NIC driver is no longer compiled into the kernel. It ships
   as a loadable module (kernel/modules/e1000.kxt) and is loaded at runtime
   via the .kxt framework (insmod/kxtload). See module.h / module.c. */

/* ---------------------------------------------------------------- *
 * Bootstrap
 * ---------------------------------------------------------------- */
void driver_init(void)
{
    driver_register(&ata_drv);
    driver_register(&xhci_drv);
    driver_register(&vga_drv);
    sata_driver_init();
    nvme_driver_init();

    /* Static character devices (no PCI dependence). */
    struct device *z = (struct device *)kmalloc(sizeof(struct device));
    memset(z, 0, sizeof(*z));
    dname(z->name, "zero");
    z->major = 1; z->minor = 3; z->type = DEV_CHAR;
    z->ops.read = zero_read; z->ops.write = zero_write;
    device_register(z);

    struct device *t = (struct device *)kmalloc(sizeof(struct device));
    memset(t, 0, sizeof(*t));
    dname(t->name, "ttyS0");
    t->major = 4; t->minor = 0; t->type = DEV_CHAR;
    t->ops.read = tty_read; t->ops.write = tty_write;
    device_register(t);

    /* DRI render node for Mesa softpipe (no PCI dependence). */
    dri_init();

    /* Unified GUI input queue (keyboard + mouse for axwm/axterm). */
    input_init();

    printk("DRV: framework initialized\n");

    /* Probe drivers against the devices enumerated by pci_init(). */
    driver_probe_all();
}
