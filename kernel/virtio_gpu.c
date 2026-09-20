#include "virtio_gpu.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "hal/cshim.h"
#include <stddef.h>

static struct virtio_gpu_device g_vgpu;
static struct virtio_gpu_device *gp_vgpu = &g_vgpu;

static struct pci_device *find_virtio_gpu(void)
{
    struct pci_device *p = pci_first();
    for (; p; p = p->next) {
        if (p->vendor == VIRTIO_GPU_VENDOR && p->device == VIRTIO_GPU_DEVICE)
            return p;
    }
    return NULL;
}

static uint32_t vgpu_pci_read(struct virtio_gpu_device *d, uint8_t off)
{
    return pci_read32(d->bus, d->dev, d->func, off);
}

static void vgpu_pci_write(struct virtio_gpu_device *d, uint8_t off, uint32_t val)
{
    pci_write32(d->bus, d->dev, d->func, off, val);
}

static uint16_t vgpu_pci_read16(struct virtio_gpu_device *d, uint8_t off)
{
    return pci_read16(d->bus, d->dev, d->func, off);
}

static void vgpu_pci_write16(struct virtio_gpu_device *d, uint8_t off, uint16_t val)
{
    pci_write16(d->bus, d->dev, d->func, off, val);
}

static uint8_t vgpu_pci_read8(struct virtio_gpu_device *d, uint8_t off)
{
    return pci_read8(d->bus, d->dev, d->func, off);
}

static uint32_t vgpu_pci_read32(struct virtio_gpu_device *d, uint8_t off)
{
    return pci_read32(d->bus, d->dev, d->func, off);
}

static void vgpu_pci_write32(struct virtio_gpu_device *d, uint8_t off, uint32_t val)
{
    pci_write32(d->bus, d->dev, d->func, off, val);
}

static uint32_t vgpu_dev_feature(struct virtio_gpu_device *d, uint32_t sel)
{
    vgpu_pci_write(d, VIRTIO_PCI_DEV_FEATURE_SEL, sel);
    return vgpu_pci_read(d, VIRTIO_PCI_DEV_FEATURE);
}

static void vgpu_drv_feature(struct virtio_gpu_device *d, uint32_t sel, uint32_t val)
{
    vgpu_pci_write(d, VIRTIO_PCI_DRV_FEATURE_SEL, sel);
    vgpu_pci_write(d, VIRTIO_PCI_DRV_FEATURE, val);
    vgpu_pci_write(d, VIRTIO_PCI_DEV_FEATURE_SEL, 1);
    vgpu_pci_write(d, VIRTIO_PCI_DEV_FEATURE, val);
}

static void vring_notify(struct virtio_gpu_vring *v)
{
    if (!v)
        return;
    struct virtio_gpu_device *d = gp_vgpu;
    uint32_t notify = d->common_offset + (uint32_t)d->notify_offset_multiplier * (uint32_t)v->idx;
    *(volatile uint16_t *)((uintptr_t)d->mmio + notify) = (uint16_t)v->idx;
}

static int setup_vring(struct virtio_gpu_vring *v, int idx, uint32_t entries,
                       volatile uint8_t *mmio)
{
    struct virtio_gpu_device *d = gp_vgpu;
    size_t desc_size = entries * sizeof(struct vring_desc);
    size_t avail_size = sizeof(struct vring_avail) + entries * sizeof(uint16_t);
    size_t used_size = sizeof(struct vring_used) + entries * sizeof(struct vring_used_elem);
    size_t total = desc_size + avail_size + used_size;
    uint32_t pages = (uint32_t)((total + PAGE_SIZE - 1) >> PAGE_SHIFT);

    void *phys = pmm_alloc_frames(pages);
    if (!phys)
        return -1;

    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, pages, MMU_WRITE);
    if (!virt) {
        pmm_free_frames(phys, pages);
        return -1;
    }

    v->idx = idx;
    v->entries = entries;
    v->pfn = (uint64_t)(uintptr_t)phys;
    v->num_pages = pages;
    v->mmio = mmio;
    v->avail_idx = 0;
    v->used_idx = 0;

    uint8_t *base = (uint8_t *)virt;
    v->desc = (struct vring_desc *)base;
    v->avail = (struct vring_avail *)(base + desc_size);
    v->used = (struct vring_used *)(base + desc_size + avail_size);

    memset(v->desc, 0, desc_size);
    memset(v->avail, 0, avail_size);
    memset(v->used, 0, used_size);

    uint32_t cfg = d->common_offset;
    volatile uint16_t *p16 = (volatile uint16_t *)((uintptr_t)mmio + cfg);
    volatile uint32_t *p32 = (volatile uint32_t *)((uintptr_t)mmio + cfg);

    /* Modern transport setup: select, size, enable, desc/avail/used, re-enable */
    p16[0x16 / 2] = (uint16_t)idx;
    p16[0x18 / 2] = (uint16_t)entries;
    p16[0x1A / 2] = 0; /* queue_msix_vector = 0 */
    p16[0x1C / 2] = 0;
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
    /* Enable first */
    p16[0x1C / 2] = (uint16_t)(1U << idx);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
    /* Then write addresses */
    p32[0x20 / 4] = (uint32_t)(v->pfn & 0xFFFFFFFF);
    p32[0x24 / 4] = (uint32_t)(v->pfn >> 32);
    p32[0x28 / 4] = (uint32_t)(v->pfn + desc_size);
    p32[0x2C / 4] = (uint32_t)((v->pfn + desc_size) >> 32);
    p32[0x30 / 4] = (uint32_t)(v->pfn + desc_size + avail_size);
    p32[0x34 / 4] = (uint32_t)((v->pfn + desc_size + avail_size) >> 32);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
    /* Re-disable and re-enable */
    p16[0x1C / 2] = 0;
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
    p16[0x1C / 2] = (uint16_t)(1U << idx);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
    uint16_t enable_r = p16[0x1C / 2];
    uint16_t qsel_r = p16[0x16 / 2];
    uint16_t size_r = p16[0x18 / 2];
    uint16_t msix_r = p16[0x1A / 2];
    uint32_t qused_lo = *(volatile uint32_t *)((uintptr_t)d->mmio + cfg + 0x30);
    uint32_t qused_hi = *(volatile uint32_t *)((uintptr_t)d->mmio + cfg + 0x34);
    printk("virtio-gpu: vring %d entries=%u en=%u qsel=%u size=%u msix=%u qused=0x%08x%08x\n",
           idx, entries, enable_r, qsel_r, size_r, msix_r, qused_hi, qused_lo);
    /* Write magic for mapping validation */
    *(volatile uint32_t *)((uintptr_t)d->mmio + cfg + 0x7C) = 0xDEADBEEF;
    uint32_t magic_chk = *(volatile uint32_t *)((uintptr_t)d->mmio + cfg + 0x7C);
    printk("virtio-gpu: vring %d magic_write=0x%08x magic_read=0x%08x %s\n",
           idx, 0xDEADBEEF, magic_chk, magic_chk == 0xDEADBEEF ? "OK" : "FAIL");
    return 0;
}

int virtio_gpu_probe(void)
{
    if (gp_vgpu->detected)
        return 1;
    struct pci_device *p = find_virtio_gpu();
    if (!p)
        return 0;
    gp_vgpu->detected = 1;
    gp_vgpu->bus = p->bus;
    gp_vgpu->dev = p->dev;
    gp_vgpu->func = p->func;
    gp_vgpu->mmio_phys = pci_bar_addr(p, 0);
    gp_vgpu->mmio_size = 0x10000;
    gp_vgpu->irq = p->irq;
    gp_vgpu->fb_resource_id = 0;
    gp_vgpu->fb_virt = NULL;
    gp_vgpu->common_offset = 0x2800;
    gp_vgpu->common_bar = 0;
    gp_vgpu->notify_offset_multiplier = 0x1000;
    printk("virtio-gpu: detected at %02x:%02x.%u mmio=0x%lx irq=%u\n",
           p->bus, p->dev, p->func,
           (unsigned long)gp_vgpu->mmio_phys, gp_vgpu->irq);
    return 1;
}

int virtio_gpu_init(struct virtio_gpu_device *d)
{
    (void)d;
    struct virtio_gpu_device *vgpu = gp_vgpu;

    uint32_t bar0 = vgpu_pci_read(vgpu, 0x10);
    uint32_t bar0_hi = 0;
    if (((bar0 >> 1) & 0x3) == 2)
        bar0_hi = vgpu_pci_read(vgpu, 0x14);
    uint64_t bar0_phys = ((uint64_t)bar0_hi << 32) | (bar0 & 0xFFFFFFF0);
    printk("virtio-gpu: bar0 phys=0x%016lx size=0x%lx\n",
           bar0_phys, (unsigned long)vgpu->mmio_size);

    vgpu->mmio = (volatile uint8_t *)vmm_mmap_phys(bar0_phys,
        (vgpu->mmio_size + PAGE_SIZE - 1) >> PAGE_SHIFT,
        MMU_WRITE | MMU_UNCACHED);
    if (!vgpu->mmio) {
        printk("virtio-gpu: failed to map MMIO\n");
        return -1;
    }
    printk("virtio-gpu: mmio virt=%p\n", vgpu->mmio);

    /* Read all 6 BARs */
    for (int i = 0; i < 6; i++) {
        uint32_t bar = vgpu_pci_read(vgpu, 0x10 + i * 4);
        printk("virtio-gpu: BAR%d=0x%08x", i, bar);
        if (bar & 1) {
            printk(" (IO 0x%04x)", bar & 0xFFFC);
        } else {
            uint64_t phys = bar & 0xFFFFFFF0;
            int is64 = ((bar >> 1) & 0x3) == 2;
            if (is64 && i < 5) {
                uint32_t hi = vgpu_pci_read(vgpu, 0x10 + (i + 1) * 4);
                phys = ((uint64_t)hi << 32) | phys;
                printk(" (64bit MMIO 0x%016lx)", phys);
                i++;
            } else {
                printk(" (32bit MMIO 0x%08x)", phys);
            }
        }
        printk("\n");
    }

    /* Test if MMIO mapping works at BAR0: write+readback */
    {
        volatile uint32_t *p = (volatile uint32_t *)vgpu->mmio;
        p[0] = 0xCAFEBABE;
        uint32_t r = p[0];
        printk("virtio-gpu: BAR0 writeback: wrote=0x%08x read=0x%08x %s\n",
               0xCAFEBABE, r, r == 0xCAFEBABE ? "OK" : "FAIL");
        p[1] = 0xDEADBEEF;
        r = p[1];
        printk("virtio-gpu: BAR0+4 writeback: wrote=0x%08x read=0x%08x %s\n",
               0xDEADBEEF, r, r == 0xDEADBEEF ? "OK" : "FAIL");
        /* Restore */
        p[0] = 0; p[1] = 0;
    }

    /* Check identity mapping: hal_mmio_map_phys at BAR0 */
    {
        void *id = hal_mmio_map_phys(bar0_phys, 0x1000);
        if (id) {
            uint32_t v = *(volatile uint32_t *)id;
            printk("virtio-gpu: identity@0x80000000=0x%08x (vs mmio=0x%08x)\n",
                   v, *(volatile uint32_t *)vgpu->mmio);
        }
    }

    /* Verify modern transport common config at BAR0+0x2800 */
    {
        volatile uint32_t *p = (volatile uint32_t *)vgpu->mmio;
        uint32_t cfg = vgpu->common_offset;
        if (cfg < 0x10000) {
            p[cfg / 4] = (1U << VIRTIO_GPU_F_VIRGL);
            for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
            uint32_t feat = p[(cfg + 4) / 4];
            printk("virtio-gpu: common cfg at 0x%04x: feat=0x%08x %s\n",
                   cfg, feat, feat ? "OK" : "FAIL");
            p[cfg / 4] = 0;
        }
    }
    /* Test notification at various offsets */
    {
        volatile uint8_t *base = (volatile uint8_t *)vgpu->mmio;
        uint32_t cfg = vgpu->common_offset;
        /* Read common config queue_select to find current queue */
        uint16_t qsel = *(volatile uint16_t *)((uintptr_t)base + cfg + 0x16);
        printk("virtio-gpu: qsel after setup=%u\n", qsel);
        /* Test: write queue index to various offsets and check if device responds */
        /* First, submit a command without notification to establish baseline */
        /* Check used ring at q0 physical address (0x13204) */
        /* Instead, check queue_used_lo/hi in common config (current queue) */
        uint32_t used_lo = *(volatile uint32_t *)((uintptr_t)base + cfg + 0x30);
        uint32_t used_hi = *(volatile uint32_t *)((uintptr_t)base + cfg + 0x34);
        printk("virtio-gpu: q_used_lo=0x%08x q_used_hi=0x%08x\n", used_lo, used_hi);
    }

    /* Enable device (command register bit 0 = I/O space) */
    uint32_t command = vgpu_pci_read(vgpu, VIRTIO_PCI_COMMAND);
    vgpu_pci_write(vgpu, VIRTIO_PCI_COMMAND, command | 0x1);

    /* Read all 4 feature dwords */
    uint32_t feat = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t f = vgpu_dev_feature(vgpu, i);
        feat |= (f << (i * 32));
    }
    vgpu->device_features = feat;

    /* Select features we want */
    uint32_t wanted = 0;
    if (feat & (1U << VIRTIO_GPU_F_VIRGL))
        wanted |= (1U << VIRTIO_GPU_F_VIRGL);
    if (feat & (1U << VIRTIO_GPU_F_RESOURCE_BLOB))
        wanted |= (1U << VIRTIO_GPU_F_RESOURCE_BLOB);
    if (feat & (1U << VIRTIO_GPU_F_CONTEXT_INIT))
        wanted |= (1U << VIRTIO_GPU_F_CONTEXT_INIT);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t chunk = wanted >> (i * 32);
        if (chunk)
            vgpu_drv_feature(vgpu, i, chunk);
    }
    vgpu->driver_features = wanted;

    /* Set status: ACK + DRIVER */
    uint32_t status = vgpu_pci_read(vgpu, VIRTIO_PCI_STATUS);
    vgpu_pci_write(vgpu, VIRTIO_PCI_STATUS,
                   status | VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Allocate identity-mapped command/response buffers.
       Device reads descriptor addresses as PHYSICAL addresses;
       stack/higher-half virts would be wrong. */
    vgpu->cmd_phys = (uint64_t)(uintptr_t)pmm_alloc_frame();
    if (!vgpu->cmd_phys) {
        printk("virtio-gpu: failed to alloc cmd buffer\n");
        return -1;
    }
    vgpu->cmd_virt = (void *)vgpu->cmd_phys;

    vgpu->resp_phys = (uint64_t)(uintptr_t)pmm_alloc_frame();
    if (!vgpu->resp_phys) {
        pmm_free_frame((void *)(uintptr_t)vgpu->cmd_phys);
        printk("virtio-gpu: failed to alloc resp buffer\n");
        return -1;
    }
    vgpu->resp_virt = (void *)vgpu->resp_phys;

    /* Small delay */
    for (volatile int i = 0; i < 100000; i++)
        __asm__ volatile("pause" ::: "memory");

    /* Set status: DRIVER_OK + FEATURES_OK */
    status = vgpu_pci_read(vgpu, VIRTIO_PCI_STATUS);
    vgpu_pci_write(vgpu, VIRTIO_PCI_STATUS,
                   status | VIRTIO_STATUS_DRIVER_OK | VIRTIO_STATUS_FEATURES_OK);

    printk("virtio-gpu: features=0x%08x wanted=0x%08x\n", feat, wanted);

    /* Set up queues AFTER DRIVER_OK (re-enable sequence works) */
    if (setup_vring(&vgpu->control, 0, VIRTIO_GPU_MAX_VRING_ENTRIES, vgpu->mmio) < 0) {
        printk("virtio-gpu: failed to set up control vring\n");
        return -1;
    }

    if (setup_vring(&vgpu->scanout, 1, 64, vgpu->mmio) < 0) {
        printk("virtio-gpu: failed to set up scanout vring\n");
        return -1;
    }

    vgpu->initialized = 1;
    printk("virtio-gpu: transport ready cmd=0x%lx resp=0x%lx\n",
           vgpu->cmd_phys, vgpu->resp_phys);
    return 0;
}

void virtio_gpu_cleanup(struct virtio_gpu_device *vgpu)
{
    struct virtio_gpu_device *d = gp_vgpu;
    (void)vgpu;

    if (d->fb_virt) {
        vmm_unmap_page((uint64_t)(uintptr_t)d->fb_virt);
        if (d->fb_size)
            pmm_free_frames((void *)(uintptr_t)d->fb_phys, d->fb_size >> PAGE_SHIFT);
        d->fb_virt = NULL;
        d->fb_phys = 0;
        d->fb_size = 0;
    }

    if (d->control.pfn) {
        vmm_unmap_page(d->control.pfn);
        pmm_free_frames((void *)(uintptr_t)d->control.pfn, d->control.num_pages);
        memset(&d->control, 0, sizeof(d->control));
    }

    if (d->scanout.pfn) {
        vmm_unmap_page(d->scanout.pfn);
        pmm_free_frames((void *)(uintptr_t)d->scanout.pfn, d->scanout.num_pages);
        memset(&d->scanout, 0, sizeof(d->scanout));
    }

    if (d->resp_phys) {
        pmm_free_frame((void *)(uintptr_t)d->resp_phys);
        d->resp_phys = 0;
        d->resp_virt = NULL;
    }
    if (d->cmd_phys) {
        pmm_free_frame((void *)(uintptr_t)d->cmd_phys);
        d->cmd_phys = 0;
        d->cmd_virt = NULL;
    }

    d->initialized = 0;
    d->detected = 0;
}

int virtio_gpu_submit(struct virtio_gpu_device *vgpu,
                            void *cmd, size_t cmd_len,
                            void *resp, size_t resp_len,
                            int timeout_ms)
{
    (void)timeout_ms;
    struct virtio_gpu_vring *v = &vgpu->control;

    /* Check vring physical mapping */
    {
        uint64_t desc_phys = vmm_virt_to_phys((uint64_t)v->desc);
        uint64_t avail_phys = vmm_virt_to_phys((uint64_t)v->avail);
        uint64_t used_phys = vmm_virt_to_phys((uint64_t)v->used);
        printk("virtio-gpu: vring phys: desc=0x%lx avail=0x%lx used=0x%lx\n",
               (unsigned long)desc_phys, (unsigned long)avail_phys, (unsigned long)used_phys);
    }

    /* Check COMMAND register */
    {
        uint32_t cmd = vgpu_pci_read(vgpu, VIRTIO_PCI_COMMAND);
        if (!(cmd & 0x4)) {
            printk("virtio-gpu: enabling bus master\n");
            vgpu_pci_write(vgpu, VIRTIO_PCI_COMMAND, cmd | 0x4);
        }
    }

    /* Re-apply queue config before submit */
    {
        volatile uint16_t *p16 = (volatile uint16_t *)((uintptr_t)vgpu->mmio + vgpu->common_offset);
        volatile uint32_t *p32 = (volatile uint32_t *)((uintptr_t)vgpu->mmio + vgpu->common_offset);
        p16[0x16 / 2] = 0;
        p16[0x18 / 2] = v->entries;
        p32[0x20 / 4] = (uint32_t)(v->pfn & 0xFFFFFFFF);
        p32[0x24 / 4] = (uint32_t)(v->pfn >> 32);
        p32[0x28 / 4] = (uint32_t)(v->pfn + 4096);
        p32[0x2C / 4] = 0;
        p32[0x30 / 4] = (uint32_t)(v->pfn + 4096 + 516);
        p32[0x34 / 4] = 0;
        p16[0x1C / 2] = 1;
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        uint16_t qen_chk = p16[0x1C / 2];
        if (qen_chk != 1) {
            p16[0x1C / 2] = 0;
            for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
            p16[0x1C / 2] = 1;
            for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        }
    }

    /* Find a free available ring slot */
    uint16_t head = v->avail_idx;
    if (head >= v->entries)
        return -1;

    /* Copy command to device-internal buffer (identity-mapped,
       so virtual == physical; safe for device DMA). */
    __builtin_memcpy(vgpu->cmd_virt, cmd, cmd_len);

    /* Write command and response descriptors with PHYSICAL addresses:
       desc[head] = cmd (read), desc[head+1] = resp (write) */
    uint16_t desc_cmd_idx = head;
    uint16_t desc_resp_idx = (uint16_t)(head + 1);

    struct vring_desc *dcmd = &v->desc[desc_cmd_idx];
    dcmd->addr = vgpu->cmd_phys;
    dcmd->len = (uint32_t)cmd_len;
    dcmd->flags = VIRTIO_DESC_F_NEXT;
    dcmd->next = desc_resp_idx;

    struct vring_desc *dresp = &v->desc[desc_resp_idx];
    dresp->addr = vgpu->resp_phys;
    dresp->len = (uint32_t)resp_len;
    dresp->flags = VIRTIO_DESC_F_WRITE;
    dresp->next = 0xFFFF;

    v->avail->ring[head % v->entries] = desc_cmd_idx;
    v->avail->idx = head + 1;
    v->avail_idx = head + 1;

    printk("virtio-gpu: submit head=%u desc=%u,%u cmd_pfn=0x%lx len=%u\n",
           head, desc_cmd_idx, desc_resp_idx,
           (unsigned long)(vgpu->cmd_phys), (unsigned)cmd_len);

    /* Re-apply queue config before submit */
    {
        struct virtio_gpu_vring *v = &vgpu->control;
        volatile uint16_t *p16 = (volatile uint16_t *)((uintptr_t)vgpu->mmio + vgpu->common_offset);
        volatile uint32_t *p32 = (volatile uint32_t *)((uintptr_t)vgpu->mmio + vgpu->common_offset);
        p16[0x16 / 2] = 0;
        p16[0x18 / 2] = v->entries;
        p16[0x1C / 2] = 0;
        p32[0x20 / 4] = (uint32_t)(v->pfn & 0xFFFFFFFF);
        p32[0x24 / 4] = (uint32_t)(v->pfn >> 32);
        p32[0x28 / 4] = (uint32_t)(v->pfn + 4096);
        p32[0x2C / 4] = 0;
        p32[0x30 / 4] = (uint32_t)(v->pfn + 4096 + 516);
        p32[0x34 / 4] = 0;
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        p16[0x1C / 2] = 1;
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        p16[0x1C / 2] = 0;
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        p16[0x1C / 2] = 1;
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause" ::: "memory");
        /* Check stability immediately after */
        for (int k = 0; k < 5; k++) {
            uint16_t qsel = p16[0x16 / 2];
            uint16_t qen = p16[0x1C / 2];
            uint32_t qused = p32[0x30 / 4];
            printk("virtio-gpu: stab %d qsel=%u qen=%u qused=0x%08x\n", k, qsel, qen, qused);
            for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause" ::: "memory");
        }
    }

    vring_notify(v);

    /* Read back avail ring */
    uint16_t avail_idx_r = v->avail->idx;
    uint16_t avail_ring0 = v->avail->ring[0];
    printk("virtio-gpu: avail_r idx=%u ring0=%u used_idx=%u\n",
           avail_idx_r, avail_ring0, v->used->idx);

    /* Poll used ring for completion */
    int waited = 0;
    while (v->used->idx == 0) {
        if (waited % 10000 == 0) {
            uint32_t qused_chk = *(volatile uint32_t *)((uintptr_t)vgpu->mmio + vgpu->common_offset + 0x30);
            printk("virtio-gpu: poll %d qused_dev=0x%08x\n", waited, qused_chk);
        }
        if (waited > 100000) {
            printk("virtio-gpu: submit timeout\n");
            return -1;
        }
        __asm__ volatile("pause" ::: "memory");
        waited++;
    }

    /* Find our descriptor completion in the used ring */
    for (uint16_t i = 0; i < v->used->idx; i++) {
        struct vring_used_elem *elem = &v->used->ring[i % v->entries];
        if (elem->id == desc_cmd_idx || elem->id == desc_resp_idx) {
            if (i >= v->used_idx) {
                v->used_idx = i + 1;
            }
            break;
        }
    }

    /* Copy response back to caller */
    if (resp && resp_len) {
        __builtin_memcpy(resp, vgpu->resp_virt, resp_len);
    }

    /* Check response header for error */
    if (resp_len >= sizeof(struct virtio_gpu_ctrl_hdr)) {
        struct virtio_gpu_ctrl_hdr *rh = (struct virtio_gpu_ctrl_hdr *)resp;
        if (rh->type >= 0x1200)
            return -1;
    }

    return 0;
}

struct virtio_gpu_device *virtio_gpu_dev(void)
{
    return gp_vgpu;
}
