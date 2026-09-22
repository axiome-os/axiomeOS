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
#include <stdbool.h>

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
    bool is_modern = (d->common_bar != 0 || d->notify_bar != 0);
    if (is_modern) {
        volatile uint8_t *common = d->mmio + d->common_offset;
        *(volatile uint32_t *)(common + 0x00) = sel;
        __asm__ volatile("" ::: "memory");
        return *(volatile uint32_t *)(common + 0x04);
    }
    vgpu_pci_write(d, VIRTIO_PCI_DEV_FEATURE_SEL, sel);
    return vgpu_pci_read(d, VIRTIO_PCI_DEV_FEATURE);
}

static void vgpu_drv_feature(struct virtio_gpu_device *d, uint32_t sel, uint32_t val)
{
    bool is_modern = (d->common_bar != 0 || d->notify_bar != 0);
    if (is_modern) {
        volatile uint8_t *common = d->mmio + d->common_offset;
        *(volatile uint32_t *)(common + 0x08) = sel;
        *(volatile uint32_t *)(common + 0x0C) = val;
        __asm__ volatile("" ::: "memory");
        return;
    }
    vgpu_pci_write(d, VIRTIO_PCI_DRV_FEATURE_SEL, sel);
    vgpu_pci_write(d, VIRTIO_PCI_DRV_FEATURE, val);
}

static uint8_t vgpu_read_status(struct virtio_gpu_device *d)
{
    bool is_modern = (d->common_bar != 0 || d->notify_bar != 0);
    if (is_modern) {
        return *(volatile uint8_t *)(d->mmio + d->common_offset + 0x14);
    }
    return vgpu_pci_read8(d, VIRTIO_PCI_STATUS);
}

static void vgpu_write_status(struct virtio_gpu_device *d, uint8_t val)
{
    bool is_modern = (d->common_bar != 0 || d->notify_bar != 0);
    if (is_modern) {
        *(volatile uint8_t *)(d->mmio + d->common_offset + 0x14) = val;
        __asm__ volatile("" ::: "memory");
    } else {
        pci_write8(d->bus, d->dev, d->func, VIRTIO_PCI_STATUS, val);
    }
}

/* byte-wide PCI helpers for status */
static void vgpu_pci_write8(struct virtio_gpu_device *d, uint8_t off, uint8_t val)
{
    pci_write8(d->bus, d->dev, d->func, off, val);
}

static void vring_notify(struct virtio_gpu_vring *v)
{
    if (!v)
        return;
    struct virtio_gpu_device *d = gp_vgpu;
    bool modern = (d->driver_features & (1ULL << VIRTIO_F_VERSION_1)) != 0;
    if (modern && d->notify_base) {
        uint32_t off = (uint32_t)v->notify_off * d->notify_offset_multiplier;
        volatile uint16_t *n = (volatile uint16_t *)(d->notify_base + off);
        *n = (uint16_t)v->idx;
        __asm__ volatile("" ::: "memory");
        return;
    }
    vgpu_pci_write16(d, VIRTIO_PCI_QUEUE_NOTIFY, (uint16_t)v->idx);
}

static int setup_vring_legacy(struct virtio_gpu_vring *v, int idx, uint32_t entries)
{
    struct virtio_gpu_device *d = gp_vgpu;
    size_t desc_size = entries * sizeof(struct vring_desc);
    size_t avail_size = sizeof(struct vring_avail) + entries * sizeof(uint16_t);
    size_t used_size = sizeof(struct vring_used) + entries * sizeof(struct vring_used_elem);
    size_t total = desc_size + avail_size + used_size;
    total = (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t pages = (uint32_t)(total >> PAGE_SHIFT);

    // Proper DMA32: QEMU virtio needs <4G and >1M (avoid BIOS alias).
    // Hog is probabilistic bruteforce – use dedicated DMA32 allocator.
    void *phys = pmm_alloc_frames_high(pages, 0x100000000ULL);
    if (!phys) phys = pmm_alloc_frames(pages); // fallback if fragmented
    if (!phys)
        return -1;
    // Safety: if DMA32 still gave <1M (should not happen after pmm reserve), retry high
    if ((uintptr_t)phys < 0x100000) {
        void *phys2 = pmm_alloc_frames_high(pages, 0x100000000ULL);
        if (phys2 && (uintptr_t)phys2 >= 0x100000) {
            pmm_free_frames(phys, pages);
            phys = phys2;
        } else if (phys2) {
            pmm_free_frames(phys2, pages);
        }
    }
    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, pages, MMU_WRITE | MMU_UNCACHED);
    if (!virt) {
        pmm_free_frames(phys, pages);
        return -1;
    }
    v->idx = idx;
    v->entries = entries;
    v->pfn = (uint64_t)(uintptr_t)phys;
    v->num_pages = pages;
    v->virt = virt;
    v->mmio = NULL;
    v->avail_idx = 0;
    v->used_idx = 0;
    v->notify_off = 0;
    uint8_t *base = (uint8_t *)virt;
    v->desc = (struct vring_desc *)base;
    v->avail = (struct vring_avail *)(base + desc_size);
    v->used = (struct vring_used *)(base + desc_size + avail_size);
    memset(v->desc, 0, desc_size);
    memset(v->avail, 0, avail_size);
    memset(v->used, 0, used_size);

    /* Legacy PCI: select queue then write PFN */
    vgpu_pci_write16(d, VIRTIO_PCI_QUEUE_SEL, (uint16_t)idx);
    uint32_t pfn = (uint32_t)(v->pfn >> PAGE_SHIFT);
    vgpu_pci_write(d, VIRTIO_PCI_QUEUE_PFN, pfn);
    uint32_t read_pfn = vgpu_pci_read(d, VIRTIO_PCI_QUEUE_PFN);
    printk("virtio-gpu: legacy vring %d entries=%u pfn=0x%x read=0x%x\n",
           idx, entries, pfn, read_pfn);
    if (read_pfn == 0) {
        printk("virtio-gpu: legacy vring %d PFN rejected\n", idx);
        return -1;
    }
    return 0;
}

static int setup_vring(struct virtio_gpu_vring *v, int idx, uint32_t entries,
                       volatile uint8_t *mmio)
{
    struct virtio_gpu_device *d = gp_vgpu;
    bool modern = (d->driver_features & (1ULL << VIRTIO_F_VERSION_1)) != 0;
    if (!modern) {
        return setup_vring_legacy(v, idx, entries);
    }
    uint32_t cfg = d->common_offset;
    volatile uint8_t *common = (volatile uint8_t *)((uintptr_t)mmio + cfg);
    // Query device size after allocation is done later; for now use requested
    size_t desc_size = entries * sizeof(struct vring_desc);
    size_t avail_size = sizeof(struct vring_avail) + entries * sizeof(uint16_t);
    size_t used_size = sizeof(struct vring_used) + entries * sizeof(struct vring_used_elem);
    size_t total = desc_size + avail_size + used_size;
    total = (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t pages = (uint32_t)(total >> PAGE_SHIFT);

    void *phys = pmm_alloc_frames_high(pages, 0x100000000ULL);
    if (!phys) phys = pmm_alloc_frames(pages);
    if (!phys) return -1;
    if ((uintptr_t)phys < 0x100000) {
        void *phys2 = pmm_alloc_frames_high(pages, 0x100000000ULL);
        if (phys2 && (uintptr_t)phys2 >= 0x100000) {
            pmm_free_frames(phys, pages);
            phys = phys2;
        } else if (phys2) {
            pmm_free_frames(phys2, pages);
        }
    }

    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, pages, MMU_WRITE | MMU_UNCACHED);
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
    v->virt = virt;

    // Linux vring_alloc_queue_split PAGE aligns avail and used
    // avail at desc_size aligned to PAGE_SIZE, used at next PAGE
    size_t avail_offset = (desc_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t used_offset = (avail_offset + avail_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (used_offset + used_size > total) {
        // fallback to contiguous if not enough pages (should not happen with 64 entries)
        avail_offset = desc_size;
        used_offset = desc_size + avail_size;
    }
    uint8_t *base = (uint8_t *)virt;
    v->desc = (struct vring_desc *)base;
    v->avail = (struct vring_avail *)(base + avail_offset);
    v->used = (struct vring_used *)(base + used_offset);

    memset(v->desc, 0, desc_size);
    memset((void*)v->avail, 0, avail_size);
    memset((void*)v->used, 0, used_size);
    __asm__ volatile("sfence" ::: "memory");

    *(volatile uint16_t *)(common + 0x16) = (uint16_t)idx;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause" ::: "memory");
    uint16_t dev_size = *(volatile uint16_t *)(common + 0x18);
    if (dev_size == 0) dev_size = entries;
    if (entries > dev_size) entries = dev_size;
    v->entries = entries;
    if (*(volatile uint16_t *)(common + 0x18) != entries)
        *(volatile uint16_t *)(common + 0x18) = entries;
    // Disable MSI-X for this queue like Linux vp_active_vq does (0xFFFF = no vector)
    // QEMU with msix_vector=0 expects interrupt, polling stalls if not acked
    *(volatile uint16_t *)(common + 0x1A) = 0xFFFF;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("pause" ::: "memory");

    /* cache notify offset for this queue */
    uint16_t notify_off = *(volatile uint16_t *)(common + 0x1E);
    v->notify_off = notify_off;

    uint64_t desc_addr = v->pfn;
    uint64_t avail_addr = v->pfn + avail_offset;
    uint64_t used_addr = v->pfn + used_offset;
    printk("virtio-gpu: vring %u addrs desc=0x%lx avail=0x%lx used=0x%lx\n",
           idx, (unsigned long)desc_addr, (unsigned long)avail_addr, (unsigned long)used_addr);
    *(volatile uint32_t *)(common + 0x20) = (uint32_t)(desc_addr & 0xFFFFFFFF);
    *(volatile uint32_t *)(common + 0x24) = (uint32_t)(desc_addr >> 32);
    *(volatile uint32_t *)(common + 0x28) = (uint32_t)(avail_addr & 0xFFFFFFFF);
    *(volatile uint32_t *)(common + 0x2C) = (uint32_t)(avail_addr >> 32);
    *(volatile uint32_t *)(common + 0x30) = (uint32_t)(used_addr & 0xFFFFFFFF);
    *(volatile uint32_t *)(common + 0x34) = (uint32_t)(used_addr >> 32);
    __asm__ volatile("sfence" ::: "memory");
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");
    uint32_t rd_desc_lo = *(volatile uint32_t *)(common + 0x20);
    uint32_t rd_avail_lo = *(volatile uint32_t *)(common + 0x28);
    uint32_t rd_used_lo = *(volatile uint32_t *)(common + 0x30);
    printk("virtio-gpu: vring %u readback desc_lo=0x%x avail_lo=0x%x used_lo=0x%x\n",
           idx, rd_desc_lo, rd_avail_lo, rd_used_lo);

    __asm__ volatile("sfence" ::: "memory");
    *(volatile uint16_t *)(common + 0x1C) = 1; /* queue_enable = 1 */
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");

    uint16_t enable_r = *(volatile uint16_t *)(common + 0x1C);
    uint16_t qsel_r = *(volatile uint16_t *)(common + 0x16);
    uint16_t size_r = *(volatile uint16_t *)(common + 0x18);
    uint16_t msix_r = *(volatile uint16_t *)(common + 0x1A);
    printk("virtio-gpu: vring %d entries=%u en=%u qsel=%u size=%u msix=%u notify_off=%u\n",
           idx, entries, enable_r, qsel_r, size_r, msix_r, notify_off);
    if (enable_r != 1) {
        printk("virtio-gpu: vring %d failed to enable\n", idx);
        return -1;
    }
    return 0;
}

static void virtio_gpu_find_caps(struct virtio_gpu_device *d)
{
    uint8_t cap_ptr = pci_read8(d->bus, d->dev, d->func, 0x34);
    printk("virtio-gpu: cap_ptr=0x%02x bar0=0x%08x bar1=0x%08x bar2=0x%08x bar3=0x%08x\n",
           cap_ptr,
           (unsigned)pci_bar_addr(find_virtio_gpu(), 0),
           (unsigned)pci_bar_addr(find_virtio_gpu(), 1),
           (unsigned)pci_bar_addr(find_virtio_gpu(), 2),
           (unsigned)pci_bar_addr(find_virtio_gpu(), 3));
    int loops = 0;
    while (cap_ptr && loops < 32) {
        uint8_t cap_id = pci_read8(d->bus, d->dev, d->func, cap_ptr);
        uint8_t cap_next = pci_read8(d->bus, d->dev, d->func, cap_ptr + 1);
        uint8_t cap_len = pci_read8(d->bus, d->dev, d->func, cap_ptr + 2);
        printk("virtio-gpu: cap at 0x%x id=0x%02x next=0x%02x len=%u\n", cap_ptr, cap_id, cap_next, cap_len);
        if (cap_id == 0x09) {
            uint8_t cfg_type = pci_read8(d->bus, d->dev, d->func, cap_ptr + 3);
            uint8_t bar = pci_read8(d->bus, d->dev, d->func, cap_ptr + 4);
            uint32_t offset = pci_read32(d->bus, d->dev, d->func, cap_ptr + 8);
            uint32_t length = pci_read32(d->bus, d->dev, d->func, cap_ptr + 12);
            uint32_t notify_mult = 0;
            if (cfg_type == 2) {
                notify_mult = pci_read32(d->bus, d->dev, d->func, cap_ptr + 16);
            }
            printk("virtio-gpu: cap type=%u bar=%u offset=0x%x len=0x%x mult=0x%x at 0x%x\n",
                   cfg_type, bar, offset, length, notify_mult, cap_ptr);
            if (cfg_type == 1) {
                d->common_bar = bar;
                d->common_offset = offset;
            } else if (cfg_type == 2) {
                d->notify_bar = bar;
                d->notify_offset = offset;
                d->notify_offset_multiplier = notify_mult ? notify_mult : 0x1000;
            } else if (cfg_type == 4) {
                d->device_bar = bar;
                d->device_offset = offset;
            }
        }
        cap_ptr = cap_next;
        loops++;
    }
    if (cap_ptr == 0 && loops == 0) {
        printk("virtio-gpu: no caps found, legacy mode\n");
    }
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
    gp_vgpu->notify_offset = 0x3000;
    gp_vgpu->device_offset = 0;
    gp_vgpu->device_bar = 0;
    gp_vgpu->next_resource_id = 1;
    gp_vgpu->next_context_id = 1;
    gp_vgpu->fence_id = 1;
    // Try to discover virtio caps (modern)
    virtio_gpu_find_caps(gp_vgpu);
    // If caps found, override mmio_phys to common BAR's phys
    if (gp_vgpu->common_bar < 6) {
        uint64_t common_phys = pci_bar_addr(p, gp_vgpu->common_bar);
        if (common_phys) {
            printk("virtio-gpu: caps common bar=%u phys=0x%lx off=0x%x\n",
                   gp_vgpu->common_bar, (unsigned long)common_phys, gp_vgpu->common_offset);
            // For modern, mmio should be common BAR's base
            // Keep mmio_phys as that BAR for mapping
            if (gp_vgpu->common_bar != 0) {
                gp_vgpu->mmio_phys = common_phys;
                printk("virtio-gpu: using common BAR %u as mmio_phys 0x%lx\n",
                       gp_vgpu->common_bar, (unsigned long)common_phys);
            }
        }
    }
    printk("virtio-gpu: detected at %02x:%02x.%u mmio=0x%lx irq=%u common off=0x%x notify off=0x%x mult=0x%x\n",
           p->bus, p->dev, p->func,
           (unsigned long)gp_vgpu->mmio_phys, gp_vgpu->irq,
           gp_vgpu->common_offset, gp_vgpu->notify_offset, gp_vgpu->notify_offset_multiplier);
    return 1;
}

int virtio_gpu_init(struct virtio_gpu_device *d)
{
    (void)d;
    struct virtio_gpu_device *vgpu = gp_vgpu;

    uint64_t bar_phys = vgpu->mmio_phys;
    // If modern caps provided a common BAR, mmio_phys already is that BAR's phys
    // Otherwise fallback to BAR0 read
    if (bar_phys == 0) {
        uint32_t bar0 = vgpu_pci_read(vgpu, 0x10);
        uint32_t bar0_hi = 0;
        if (((bar0 >> 1) & 0x3) == 2)
            bar0_hi = vgpu_pci_read(vgpu, 0x14);
        bar_phys = ((uint64_t)bar0_hi << 32) | (bar0 & 0xFFFFFFF0);
    }
    printk("virtio-gpu: bar phys=0x%016lx size=0x%lx common bar=%u off=0x%x notify bar off=0x%x mult=0x%x\n",
           bar_phys, (unsigned long)vgpu->mmio_size,
           vgpu->common_bar, vgpu->common_offset, vgpu->notify_offset, vgpu->notify_offset_multiplier);

    vgpu->mmio = (volatile uint8_t *)vmm_mmap_phys(bar_phys,
        (vgpu->mmio_size + PAGE_SIZE - 1) >> PAGE_SHIFT,
        MMU_WRITE | MMU_UNCACHED);
    if (!vgpu->mmio) {
        printk("virtio-gpu: failed to map MMIO\n");
        return -1;
    }
    // Map notify BAR separately if different from common
    if (vgpu->notify_bar != vgpu->common_bar && vgpu->notify_bar < 6) {
        uint64_t notify_phys = pci_bar_addr(find_virtio_gpu(), vgpu->notify_bar);
        if (notify_phys) {
            vgpu->notify_bar_mapped = (volatile uint8_t *)vmm_mmap_phys(notify_phys,
                (0x10000 + PAGE_SIZE - 1) >> PAGE_SHIFT, MMU_WRITE | MMU_UNCACHED);
            vgpu->notify_base = vgpu->notify_bar_mapped + vgpu->notify_offset;
            printk("virtio-gpu: notify BAR %u phys 0x%lx mapped %p\n",
                   vgpu->notify_bar, (unsigned long)notify_phys, vgpu->notify_base);
        } else {
            vgpu->notify_base = vgpu->mmio + vgpu->notify_offset;
        }
    } else {
        vgpu->notify_base = vgpu->mmio + vgpu->notify_offset;
    }
    printk("virtio-gpu: mmio virt=%p notify=%p (bar %u off 0x%x mult 0x%x)\n",
           vgpu->mmio, vgpu->notify_base, vgpu->notify_bar, vgpu->notify_offset, vgpu->notify_offset_multiplier);

    /* Enable Memory Space + Bus Master (0x2 + 0x4), not just I/O */
    uint32_t command = vgpu_pci_read(vgpu, VIRTIO_PCI_COMMAND);
    vgpu_pci_write(vgpu, VIRTIO_PCI_COMMAND, command | 0x06);
    command = vgpu_pci_read(vgpu, VIRTIO_PCI_COMMAND);
    printk("virtio-gpu: PCI command=0x%08x\n", command);

    uint8_t cur_status = vgpu_read_status(vgpu);
    printk("virtio-gpu: initial status=0x%02x\n", cur_status);
    // Linux always resets per spec (virtio_reset_device), never reuses OVMF vrings.
    // Bruteforce reuse caused mismatched driver_features/used_idx and stale cmd_virt.
    vgpu_write_status(vgpu, 0);
    for (volatile int i = 0; i < 5000; i++) __asm__ volatile("pause" ::: "memory");
    cur_status = 0;
    cur_status |= VIRTIO_STATUS_ACK;
    vgpu_write_status(vgpu, cur_status);
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");
    cur_status |= VIRTIO_STATUS_DRIVER;
    vgpu_write_status(vgpu, cur_status);
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");

    /* Read 64-bit features (up to 2 dwords needed for VERSION_1 at bit 32) */
    uint64_t feat = 0;
    uint64_t feat_pci = 0;
    uint64_t feat_mmio = 0;
    /* Try MMIO common cfg first */
    if (vgpu->mmio) {
        volatile uint8_t *common = vgpu->mmio + vgpu->common_offset;
        for (uint32_t i = 0; i < 2; i++) {
            *(volatile uint32_t *)(common + 0x00) = i;
            __asm__ volatile("" ::: "memory");
            uint32_t f = *(volatile uint32_t *)(common + 0x04);
            feat_mmio |= ((uint64_t)f) << (i*32);
        }
        printk("virtio-gpu: mmio features=0x%016lx\n", (unsigned long)feat_mmio);
    }
    for (uint32_t i = 0; i < 2; i++) {
        vgpu_pci_write(vgpu, VIRTIO_PCI_DEV_FEATURE_SEL, i);
        uint32_t f = vgpu_pci_read(vgpu, VIRTIO_PCI_DEV_FEATURE);
        feat_pci |= ((uint64_t)f) << (i*32);
    }
    printk("virtio-gpu: pci features=0x%016lx\n", (unsigned long)feat_pci);
    bool has_caps = (vgpu->common_bar != 0 || vgpu->common_offset != 0x2800);
    /* Prefer MMIO when modern caps found, else PCI */
    if (has_caps && feat_mmio && feat_mmio != 0xFFFFFFFFFFFFFFFFULL) {
        feat = feat_mmio;
        printk("virtio-gpu: using MMIO features (modern)\n");
    } else if (feat_pci && feat_pci != 0xFFFFFFFFFFFFFFFFULL) {
        feat = feat_pci;
        printk("virtio-gpu: using PCI features (legacy)\n");
    } else {
        feat = feat_mmio ? feat_mmio : feat_pci;
    }
    vgpu->device_features = feat;
    printk("virtio-gpu: device features selected=0x%016lx\n", (unsigned long)feat);

    // Linux: require VERSION_1, and only offer VIRGL if LE (x86 is LE, ok)
    if (!(feat & (1ULL << VIRTIO_F_VERSION_1))) {
        printk("virtio-gpu: VERSION_1 not offered, no modern transport\n");
        // Fall back to legacy PCI without VERSION_1 like Linux would -ENODEV, but we try legacy
    }
    uint64_t wanted = 0;
    if (feat & (1ULL << VIRTIO_F_VERSION_1)) wanted |= (1ULL << VIRTIO_F_VERSION_1);
    // Only negotiate GPU features we actually use. Don't set INDIRECT/EVENT_IDX
    // unless we have code to use them (Linux does). Bruteforce set caused
    // device to enable them but we poll anyway.
    if (feat & (1ULL << VIRTIO_GPU_F_VIRGL)) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        wanted |= (1ULL << VIRTIO_GPU_F_VIRGL);
#else
        printk("virtio-gpu: VIRGL requires LE, skipping\n");
#endif
    }
    if (feat & (1ULL << VIRTIO_GPU_F_EDID)) wanted |= (1ULL << VIRTIO_GPU_F_EDID);
    if (feat & (1ULL << VIRTIO_GPU_F_RESOURCE_UUID)) wanted |= (1ULL << VIRTIO_GPU_F_RESOURCE_UUID);
    if (feat & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)) wanted |= (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB);
    if (feat & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT)) wanted |= (1ULL << VIRTIO_GPU_F_CONTEXT_INIT);
    // RING features: only if we implement them. We poll, so don't need EVENT_IDX.
    // Keep INDIRECT disabled – we use 2-desc chain, not sg/indirect.

    bool is_modern = (vgpu->common_bar != 0 || vgpu->notify_bar != 0);
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t chunk = (uint32_t)(wanted >> (i * 32));
        if (chunk == 0 && wanted == 0)
            continue;
        if (is_modern) {
            volatile uint8_t *common = vgpu->mmio + vgpu->common_offset;
            *(volatile uint32_t *)(common + 0x08) = i;
            *(volatile uint32_t *)(common + 0x0C) = chunk;
        } else {
            vgpu_pci_write(vgpu, VIRTIO_PCI_DRV_FEATURE_SEL, i);
            vgpu_pci_write(vgpu, VIRTIO_PCI_DRV_FEATURE, chunk);
        }
    }
    vgpu->driver_features = wanted;

    /* FEATURES_OK */
    cur_status |= VIRTIO_STATUS_FEATURES_OK;
    vgpu_write_status(vgpu, cur_status);
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");
    uint8_t status = vgpu_read_status(vgpu);
    printk("virtio-gpu: after FEATURES_OK status=0x%02x cur=0x%02x\n", status, cur_status);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        printk("virtio-gpu: FEATURES_OK rejected (status=0x%02x cur=0x%02x feat=0x%lx wanted=0x%lx)\n",
               status, cur_status, (unsigned long)feat, (unsigned long)wanted);
        vgpu_write_status(vgpu, VIRTIO_STATUS_FAILED);
        return -1;
    }
    cur_status = status; /* device may have cleared unknown bits */

    // Read device config like Linux virtio_cread_le(num_scanouts/num_capsets/blob_alignment)
    // For modern, device config at mmio+device_offset (BAR2), for legacy at mmio+0x100?
    if (vgpu->mmio) {
        volatile uint8_t *dev_cfg = NULL;
        if (vgpu->device_bar == vgpu->common_bar) {
            dev_cfg = vgpu->mmio + vgpu->device_offset;
        } else if (vgpu->device_bar < 6) {
            // Separate BAR already mapped as notify_bar_mapped? Use mmio if same, else try to map
            // For now, try to read via PCI config if modern fails
            dev_cfg = vgpu->mmio + vgpu->device_offset;
        }
        if (dev_cfg) {
            struct virtio_gpu_config *cfg = (struct virtio_gpu_config *)dev_cfg;
            vgpu->num_scanouts = cfg->num_scanouts;
            vgpu->num_capsets = cfg->num_capsets;
            vgpu->blob_alignment = cfg->blob_alignment;
            printk("virtio-gpu: config num_scanouts=%u num_capsets=%u blob_align=%u\n",
                   vgpu->num_scanouts, vgpu->num_capsets, vgpu->blob_alignment);
            if (vgpu->has_resource_blob && vgpu->blob_alignment == 0) {
                vgpu->blob_alignment = 4096; // default
            }
        }
    }
    // Fallback if config read gave 0 (legacy or unmapped) - keep previous
    if (vgpu->num_scanouts == 0) vgpu->num_scanouts = 1;
    // Don't fail if num_capsets==0 - host may have virgl but no capset exposed (like QEMU without virglrenderer)
    // Linux would still has_virgl=1 but capset query would fail; we keep has_virgl for Mesa probe.

    /* Allocate command/response buffers - DMA32 proper (>1M, <4G) */
    void *cmd_phys_ptr = pmm_alloc_frames_high(1, 0x100000000ULL);
    if (!cmd_phys_ptr) cmd_phys_ptr = pmm_alloc_frame();
    if ((uintptr_t)cmd_phys_ptr && (uintptr_t)cmd_phys_ptr < 0x100000) {
        void *cmd2 = pmm_alloc_frames_high(1, 0x100000000ULL);
        if (cmd2 && (uintptr_t)cmd2 >= 0x100000) {
            pmm_free_frame(cmd_phys_ptr);
            cmd_phys_ptr = cmd2;
        } else if (cmd2) {
            pmm_free_frame(cmd2);
        }
    }
    if (!cmd_phys_ptr) {
        printk("virtio-gpu: failed to alloc cmd buffer\n");
        return -1;
    }
    vgpu->cmd_phys = (uint64_t)(uintptr_t)cmd_phys_ptr;
    vgpu->cmd_virt = vmm_mmap_phys(vgpu->cmd_phys, 1, MMU_WRITE | MMU_UNCACHED);
    if (!vgpu->cmd_virt) {
        pmm_free_frame(cmd_phys_ptr);
        printk("virtio-gpu: failed to map cmd buffer\n");
        return -1;
    }
    void *resp_phys_ptr = pmm_alloc_frames_high(1, 0x100000000ULL);
    if (!resp_phys_ptr) resp_phys_ptr = pmm_alloc_frame();
    if ((uintptr_t)resp_phys_ptr && (uintptr_t)resp_phys_ptr < 0x100000) {
        void *resp2 = pmm_alloc_frames_high(1, 0x100000000ULL);
        if (resp2 && (uintptr_t)resp2 >= 0x100000) {
            pmm_free_frame(resp_phys_ptr);
            resp_phys_ptr = resp2;
        } else if (resp2) {
            pmm_free_frame(resp2);
        }
    }
    if (!resp_phys_ptr) {
        vmm_unmap_page((uint64_t)(uintptr_t)vgpu->cmd_virt);
        pmm_free_frame(cmd_phys_ptr);
        printk("virtio-gpu: failed to alloc resp buffer\n");
        return -1;
    }
    if (!resp_phys_ptr) {
        vmm_unmap_page((uint64_t)(uintptr_t)vgpu->cmd_virt);
        pmm_free_frame(cmd_phys_ptr);
        printk("virtio-gpu: failed to alloc resp buffer\n");
        return -1;
    }
    vgpu->resp_phys = (uint64_t)(uintptr_t)resp_phys_ptr;
    vgpu->resp_virt = vmm_mmap_phys(vgpu->resp_phys, 1, MMU_WRITE | MMU_UNCACHED);
    if (!vgpu->resp_virt) {
        vmm_unmap_page((uint64_t)(uintptr_t)vgpu->cmd_virt);
        pmm_free_frame(cmd_phys_ptr);
        pmm_free_frame(resp_phys_ptr);
        printk("virtio-gpu: failed to map resp buffer\n");
        return -1;
    }

    /* Set up queues BEFORE DRIVER_OK per spec – device must see queues while FEATURES_OK */
    if (setup_vring(&vgpu->control, 0, 64, vgpu->mmio) < 0) {
        printk("virtio-gpu: failed to set up control vring\n");
        return -1;
    }
    // Cursor queue is optional - don't fail if it can't be set up
    // setup_vring(&vgpu->cursor, 1, 64, vgpu->mmio);

    /* Cache has_* flags from wanted */
    vgpu->has_virgl_3d = (wanted & (1ULL << VIRTIO_GPU_F_VIRGL)) != 0;
    vgpu->has_edid = (wanted & (1ULL << VIRTIO_GPU_F_EDID)) != 0;
    vgpu->has_resource_blob = (wanted & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)) != 0;
    vgpu->has_context_init = (wanted & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT)) != 0;
    vgpu->has_indirect = (wanted & (1ULL << VIRTIO_RING_F_INDIRECT_DESC)) != 0;
    vgpu->has_blob_alignment = (wanted & (1ULL << VIRTIO_GPU_F_BLOB_ALIGNMENT)) != 0;
    if (vgpu->has_blob_alignment) {
        /* read blob_alignment from device config at device_offset */
        /* For now keep default 0 – will be read via config if needed */
    }

    /* DRIVER_OK */
    cur_status |= VIRTIO_STATUS_DRIVER_OK;
    vgpu_write_status(vgpu, cur_status);
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause" ::: "memory");
    status = vgpu_read_status(vgpu);
    printk("virtio-gpu: status after DRIVER_OK=0x%02x cur=0x%02x\n", status, cur_status);

    printk("virtio-gpu: features dev=0x%016lx wanted=0x%016lx has_virgl=%d blob=%d ctx=%d indirect=%d\n",
           (unsigned long)feat, (unsigned long)wanted,
           vgpu->has_virgl_3d, vgpu->has_resource_blob, vgpu->has_context_init, vgpu->has_indirect);

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

    if (d->cursor.pfn) {
        vmm_unmap_page(d->cursor.pfn);
        pmm_free_frames((void *)(uintptr_t)d->cursor.pfn, d->cursor.num_pages);
        memset(&d->cursor, 0, sizeof(d->cursor));
    }

    if (d->scanout.pfn) {
        vmm_unmap_page(d->scanout.pfn);
        pmm_free_frames((void *)(uintptr_t)d->scanout.pfn, d->scanout.num_pages);
        memset(&d->scanout, 0, sizeof(d->scanout));
    }

    if (d->resp_phys) {
        if (d->resp_virt) vmm_unmap_page((uint64_t)(uintptr_t)d->resp_virt);
        pmm_free_frame((void *)(uintptr_t)d->resp_phys);
        d->resp_phys = 0;
        d->resp_virt = NULL;
    }
    if (d->cmd_phys) {
        if (d->cmd_virt) vmm_unmap_page((uint64_t)(uintptr_t)d->cmd_virt);
        pmm_free_frame((void *)(uintptr_t)d->cmd_phys);
        d->cmd_phys = 0;
        d->cmd_virt = NULL;
    }
    if (d->control.virt) {
        vmm_unmap_page((uint64_t)(uintptr_t)d->control.virt);
        // pmm free already handled via control.pfn
    }
    if (d->cursor.virt) {
        vmm_unmap_page((uint64_t)(uintptr_t)d->cursor.virt);
    }

    d->initialized = 0;
    /* keep detected so probe not rerun, but allow re-init */
}

int virtio_gpu_submit(struct virtio_gpu_device *vgpu,
                            void *cmd, size_t cmd_len,
                            void *resp, size_t resp_len,
                            int timeout_ms)
{
    struct virtio_gpu_vring *v = &vgpu->control;
    if (!vgpu->initialized || !v->desc)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 2000;

    /* Ensure bus master */
    uint32_t pcicmd = vgpu_pci_read(vgpu, VIRTIO_PCI_COMMAND);
    if (!(pcicmd & 0x04)) {
        vgpu_pci_write(vgpu, VIRTIO_PCI_COMMAND, pcicmd | 0x04);
    }

    /* synchronous single-command: reuse descs 0 and 1, wait for completion */
    uint16_t last_used = v->used_idx;
    uint16_t last_avail = v->avail->idx;
    uint32_t _cmd_type = ((struct virtio_gpu_ctrl_hdr*)cmd)->type;
    // Spammy FLUSH/TRANSFER at 60Hz would flood serial and stall the compositor.
    // Only log non-flush commands or errors verbosely.
    if (_cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH &&
        _cmd_type != VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D &&
        _cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH_ALT) {
        printk("virtio-gpu: submit cmd=0x%x len=%u avail %u->%u used %u notify off=%u\n",
               _cmd_type, (unsigned)cmd_len, last_avail, last_avail+1, last_used, v->notify_off);
    }
    /* Copy command to identity-mapped cmd buffer - ensure DMA coherent */
    __builtin_memcpy(vgpu->cmd_virt, cmd, cmd_len);
    __builtin_memset(vgpu->resp_virt, 0, resp_len);
    // Like xhci.c: clflush+mfence for DMA - ensure device sees cmd before avail
    __asm__ volatile("clflush (%0)" :: "r"(vgpu->cmd_virt) : "memory");
    __asm__ volatile("sfence" ::: "memory");

    /* descriptors 0 = cmd, 1 = resp (next chain) */
    v->desc[0].addr = vgpu->cmd_phys;
    v->desc[0].len = (uint32_t)cmd_len;
    v->desc[0].flags = VIRTIO_DESC_F_NEXT;
    v->desc[0].next = 1;
    v->desc[1].addr = vgpu->resp_phys;
    v->desc[1].len = (uint32_t)resp_len;
    v->desc[1].flags = VIRTIO_DESC_F_WRITE;
    v->desc[1].next = 0;
    __asm__ volatile("sfence" ::: "memory");

    uint16_t avail_slot = v->avail_idx % v->entries;
    v->avail->ring[avail_slot] = 0; /* head = desc 0 */
    __asm__ volatile("sfence" ::: "memory");
    v->avail->idx = v->avail_idx + 1;
    v->avail_idx++;
    __asm__ volatile("sfence" ::: "memory");
    if (_cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH &&
        _cmd_type != VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D &&
        _cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH_ALT) {
        printk("virtio-gpu: after avail idx=%u ring[0]=%u desc0 addr=0x%lx len=%u\n",
               v->avail->idx, v->avail->ring[0], (unsigned long)v->desc[0].addr, v->desc[0].len);
    }

    vring_notify(v);
    if (_cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH &&
        _cmd_type != VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D &&
        _cmd_type != VIRTIO_GPU_CMD_RESOURCE_FLUSH_ALT) {
        printk("virtio-gpu: notified queue %u\n", v->idx);
    }

    /* poll used ring with timeout - volatile like xHCI, with mfence */
    int waited = 0;
    int timeout_ticks = timeout_ms * 1000; /* ~1us per iteration */
    while (*(volatile uint16_t*)&v->used->idx == last_used) {
        __asm__ volatile("pause" ::: "memory");
        waited++;
        if (waited > timeout_ticks) {
            printk("virtio-gpu: submit timeout (cmd=0x%x used %u -> %u)\n",
                   ((struct virtio_gpu_ctrl_hdr*)cmd)->type, last_used, *(volatile uint16_t*)&v->used->idx);
            return -1;
        }
    }
    __asm__ volatile("lfence" ::: "memory");
    v->used_idx = *(volatile uint16_t*)&v->used->idx;

    if (resp && resp_len) {
        __builtin_memcpy(resp, vgpu->resp_virt, resp_len);
    }

    if (resp_len >= sizeof(struct virtio_gpu_ctrl_hdr)) {
        struct virtio_gpu_ctrl_hdr *rh = (struct virtio_gpu_ctrl_hdr *)resp;
        if (rh->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
            printk("virtio-gpu: device error resp 0x%x for cmd 0x%x\n",
                   rh->type, ((struct virtio_gpu_ctrl_hdr*)cmd)->type);
            return -1;
        }
    }
    return 0;
}

uint32_t virtio_gpu_alloc_resource_id(void)
{
    struct virtio_gpu_device *d = gp_vgpu;
    return d->next_resource_id++;
}

uint32_t virtio_gpu_alloc_context_id(void)
{
    struct virtio_gpu_device *d = gp_vgpu;
    return d->next_context_id++;
}

int virtio_gpu_create_context(uint32_t ctx_id, uint32_t context_init, const char *name)
{
    struct virtio_gpu_device *vgpu = gp_vgpu;
    struct virtio_gpu_ctx_create cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.nlen = name ? (uint32_t)__builtin_strlen(name) : 0;
    if (cmd.nlen > 63) cmd.nlen = 63;
    cmd.context_init = context_init;
    if (name && cmd.nlen)
        __builtin_memcpy(cmd.debug_name, name, cmd.nlen);
    struct virtio_gpu_ctrl_hdr resp;
    return virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000);
}

int virtio_gpu_destroy_context(uint32_t ctx_id)
{
    struct virtio_gpu_device *vgpu = gp_vgpu;
    struct virtio_gpu_ctx_destroy cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;
    struct virtio_gpu_ctrl_hdr resp;
    return virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 1000);
}

int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t width, uint32_t height,
                                  uint32_t target, uint32_t format, uint32_t bind)
{
    struct virtio_gpu_device *vgpu = gp_vgpu;
    struct virtio_gpu_resource_create_3d cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.resource_id = resource_id;
    cmd.target = target ? target : 2; /* TEXTURE_2D */
    cmd.format = format ? format : VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd.bind = bind;
    cmd.width = width;
    cmd.height = height;
    cmd.depth = 1;
    cmd.array_size = 1;
    cmd.last_level = 0;
    cmd.nr_samples = 0;
    cmd.flags = 0;
    struct virtio_gpu_ctrl_hdr resp;
    return virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 2000);
}

int virtio_gpu_submit_3d(uint32_t ctx_id, void *cmd_buf, uint32_t cmd_size)
{
    struct virtio_gpu_device *vgpu = gp_vgpu;
    /* For host3d we need to send SUBMIT_3D with fence */
    struct virtio_gpu_cmd_submit *hdr;
    size_t total = sizeof(*hdr) + cmd_size;
    /* Use stack if small */
    uint8_t local[512];
    struct virtio_gpu_ctrl_hdr resp;
    void *buf;
    if (total <= sizeof(local)) {
        buf = local;
    } else {
        buf = pmm_alloc_frame();
        if (!buf) return -1;
        /* for larger cmds we need physical contig; but mesa usually small */
    }
    hdr = (struct virtio_gpu_cmd_submit *)buf;
    __builtin_memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    hdr->hdr.ctx_id = ctx_id;
    hdr->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    hdr->hdr.fence_id = vgpu->fence_id++;
    hdr->size = cmd_size;
    __builtin_memcpy((uint8_t*)buf + sizeof(*hdr), cmd_buf, cmd_size);
    int r = virtio_gpu_submit(vgpu, buf, total, &resp, sizeof(resp), 2000);
    if (total > sizeof(local))
        pmm_free_frame(buf);
    return r;
}

int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id,
                                   uint64_t offset, struct virtio_gpu_box *box,
                                   uint32_t level, uint32_t stride, uint32_t layer_stride)
{
    struct virtio_gpu_device *vgpu = gp_vgpu;
    struct virtio_gpu_transfer_host_3d cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = resource_id;
    if (box) cmd.box = *box;
    cmd.offset = offset;
    cmd.level = level;
    cmd.stride = stride;
    cmd.layer_stride = layer_stride;
    struct virtio_gpu_ctrl_hdr resp;
    return virtio_gpu_submit(vgpu, &cmd, sizeof(cmd), &resp, sizeof(resp), 1000);
}

struct virtio_gpu_device *virtio_gpu_dev(void)
{
    return gp_vgpu;
}
