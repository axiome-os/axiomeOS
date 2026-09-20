#include "virtio_gpu_display.h"
#include "../hal/hal.h"
extern "C" {
#include "../printk.h"
#include "virtio_gpu.h"
#include "../slab.h"
#include "../string.h"
#include "../pci.h"
#include "../pmm.h"
}
#include "../vmm.h"

namespace gfx {

VirtioGpuDisplay::VirtioGpuDisplay() = default;

VirtioGpuDisplay *virtio_gpu_display_create(void)
{
    static unsigned char storage[sizeof(VirtioGpuDisplay)];
    return new (storage) VirtioGpuDisplay();
}

bool VirtioGpuDisplay::probe()
{
    if (!virtio_gpu_probe())
        return false;
    struct virtio_gpu_device *d = virtio_gpu_dev();
    printk("gfx: virtio-gpu probed (PCI %02x:%02x.%u)\n",
           d->bus, d->dev, d->func);
    return true;
}

bool VirtioGpuDisplay::present_flush(void)
{
    if (!ready_ || !fb_)
        return false;

    struct virtio_gpu_resource_flush cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.hdr.flags = 0;
    cmd.hdr.fence_id = 0;
    cmd.hdr.ctx_id = 0;
    cmd.hdr.ring_idx = 0;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = cur_.width;
    cmd.r.height = cur_.height;
    cmd.resource_id = fb_resource_id_;
    cmd.padding = 0;

    struct virtio_gpu_ctrl_hdr resp;
    int r = virtio_gpu_submit(virtio_gpu_dev(), &cmd, sizeof(cmd),
                                &resp, sizeof(resp), 1000);
    if (r < 0) {
        printk("virtio-gpu: flush failed %d\n", r);
        return false;
    }
    return true;
}

bool VirtioGpuDisplay::create_framebuffer_resource(void)
{
    uint32_t h = cur_.height;
    uint32_t pitch = cur_.pitch;
    fb_size_ = pitch * h;
    fb_pages_ = (fb_size_ + PAGE_SIZE - 1) >> PAGE_SHIFT;

    void *pages = pmm_alloc_frames(fb_pages_);
    if (!pages) {
        printk("virtio-gpu: failed to alloc backing pages\n");
        return false;
    }
    fb_phys_ = (uint64_t)(uintptr_t)pages;

    fb_ = vmm_mmap_phys(fb_phys_, fb_pages_, MMU_WRITE);
    if (!fb_) {
        pmm_free_frames(pages, fb_pages_);
        fb_phys_ = 0;
        fb_pages_ = 0;
        printk("virtio-gpu: failed to map backing pages\n");
        return false;
    }
    memset(fb_, 0, fb_size_);

    size_t cmd_size = sizeof(struct virtio_gpu_resource_attach_backing) +
                      sizeof(struct virtio_gpu_mem_entry);
    struct virtio_gpu_resource_attach_backing *cmd =
        (struct virtio_gpu_resource_attach_backing *)kmalloc(cmd_size);
    if (!cmd) {
        pmm_free_frames(pages, fb_pages_);
        fb_phys_ = 0;
        fb_pages_ = 0;
        return false;
    }
    memset(cmd, 0, cmd_size);
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->resource_id = fb_resource_id_;
    cmd->nr_entries = 1;
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *)((uint8_t *)cmd + sizeof(*cmd));
    entries[0].addr = fb_phys_;
    entries[0].length = fb_size_;
    entries[0].padding = 0;

    struct virtio_gpu_resp_display_info resp;
    int r = virtio_gpu_submit(virtio_gpu_dev(), cmd, cmd_size,
                                &resp, sizeof(resp), 1000);
    kfree(cmd);
    if (r < 0) {
        printk("virtio-gpu: attach_backing failed %d\n", r);
        vmm_unmap_page(fb_phys_);
        pmm_free_frames(pages, fb_pages_);
        fb_phys_ = 0;
        fb_pages_ = 0;
        return false;
    }

    printk("virtio-gpu: backing resource %u at phys 0x%lx size %u\n",
           fb_resource_id_, (unsigned long)fb_phys_, fb_size_);
    return true;
}

bool VirtioGpuDisplay::setup_scanout(void)
{
    struct virtio_gpu_set_scanout cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.hdr.flags = 0;
    cmd.hdr.fence_id = 0;
    cmd.hdr.ctx_id = 0;
    cmd.hdr.ring_idx = 0;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = cur_.width;
    cmd.r.height = cur_.height;
    cmd.scanout_id = 0;
    cmd.resource_id = fb_resource_id_;

    struct virtio_gpu_ctrl_hdr resp;
    int r = virtio_gpu_submit(virtio_gpu_dev(), &cmd, sizeof(cmd),
                                &resp, sizeof(resp), 1000);
    if (r < 0) {
        printk("virtio-gpu: set_scanout failed %d\n", r);
        return false;
    }
    printk("virtio-gpu: scanout 0 -> resource %u %ux%u\n",
           fb_resource_id_, cur_.width, cur_.height);
    return true;
}

bool VirtioGpuDisplay::init(const GfxMode &mode)
{
    struct virtio_gpu_device *d = virtio_gpu_dev();
    if (!d->initialized) {
        if (virtio_gpu_init(d) < 0)
            return false;
    }

    if (!d->has_display_info) {
        struct virtio_gpu_ctrl_hdr cmd = {};
        cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
        cmd.flags = 0;
        cmd.fence_id = 0;
        cmd.ctx_id = 0;
        cmd.ring_idx = 0;

        struct virtio_gpu_resp_display_info resp;
        int r = virtio_gpu_submit(virtio_gpu_dev(), &cmd, sizeof(cmd),
                                    &resp, sizeof(resp), 2000);
        if (r < 0) {
            printk("virtio-gpu: get_display_info failed %d\n", r);
            virtio_gpu_cleanup(virtio_gpu_dev());
            return false;
        }

        /* Find first enabled scanout */
        for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
            if (resp.pmodes[i].enabled) {
                cur_.width = resp.pmodes[i].r.width;
                cur_.height = resp.pmodes[i].r.height;
                d->has_display_info = 1;
                d->scanout_width = cur_.width;
                d->scanout_height = cur_.height;
                break;
            }
        }

        if (!d->has_display_info) {
            /* No enabled modes, use a default */
            cur_.width = 1024;
            cur_.height = 768;
            d->has_display_info = 1;
        }

        printk("virtio-gpu: display %ux%u\n", cur_.width, cur_.height);
    } else {
        cur_.width = d->scanout_width;
        cur_.height = d->scanout_height;
    }

    /* Pitch = width * 4 (32bpp X8R8G8B8) */
    cur_.pitch = cur_.width * 4;
    cur_.format = PixelFormat::X8R8G8B8;

    /* Respect caller's mode request if specific */
    if (mode.width != 0 && mode.width != cur_.width) {
        printk("virtio-gpu: requested %ux%u not available\n",
               mode.width, mode.height);
        virtio_gpu_cleanup(virtio_gpu_dev());
        return false;
    }
    if (mode.height != 0 && mode.height != cur_.height) {
        printk("virtio-gpu: requested %ux%u not available\n",
               mode.width, mode.height);
        virtio_gpu_cleanup(virtio_gpu_dev());
        return false;
    }

    if (!create_framebuffer_resource()) {
        virtio_gpu_cleanup(virtio_gpu_dev());
        return false;
    }

    if (!setup_scanout()) {
        virtio_gpu_cleanup(virtio_gpu_dev());
        return false;
    }

    ready_ = true;
    printk("gfx: virtio-gpu online %ux%u pitch=%u\n",
           cur_.width, cur_.height, cur_.pitch);
    return true;
}

GfxCaps VirtioGpuDisplay::caps() const
{
    GfxCaps c;
    c.has_hw_fill = 1;
    c.has_hw_blit = 1;
    c.has_hw_flip = 0;
    c.has_3d = 0;
    return c;
}

void VirtioGpuDisplay::clear(uint32_t rgb)
{
    if (!ready_ || !fb_)
        return;
    uint32_t native = canonical_to_native(rgb, cur_.format);
    uint32_t *dst = (uint32_t *)fb_;
    for (uint32_t i = 0; i < cur_.height * (cur_.pitch / 4); i++)
        dst[i] = native;
    present_flush();
}

void VirtioGpuDisplay::fill_rect(const GfxRect &r, uint32_t rgb)
{
    if (!ready_ || !fb_)
        return;
    GfxRect c = r;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    uint32_t native = canonical_to_native(rgb, cur_.format);
    uint32_t bpp = 4;
    uint8_t *base = (uint8_t *)fb_;
    for (uint32_t y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * cur_.pitch +
                                    (size_t)c.x * bpp);
        for (uint32_t x = 0; x < c.w; x++)
            row[x] = native;
    }
    present_flush();
}

void VirtioGpuDisplay::blit(const GfxRect &dst, const uint32_t *src,
                              uint32_t src_pitch_px)
{
    if (!ready_ || !fb_ || !src)
        return;
    GfxRect c = dst;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    uint32_t bpp = 4;
    uint8_t *base = (uint8_t *)fb_;
    uint32_t sx = c.x - dst.x;
    uint32_t sy = c.y - dst.y;
    for (uint32_t y = 0; y < c.h; y++) {
        const uint32_t *srow = src + (size_t)(sy + y) * src_pitch_px + sx;
        uint32_t *drow = (uint32_t *)(base + (size_t)(c.y + y) * cur_.pitch +
                                       (size_t)c.x * bpp);
        for (uint32_t x = 0; x < c.w; x++)
            drow[x] = canonical_to_native(srow[x], cur_.format);
    }
    present_flush();
}

void VirtioGpuDisplay::present()
{
    present_flush();
}

} /* namespace gfx */
