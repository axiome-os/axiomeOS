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

    // Always TRANSFER for the RAM resource (even when 3D is advertised).
    // Skipping TRANSFER for host-visible VRAM caused the host GL to never
    // see updates except for the small cursor rects - hence black flicker
    // that only appeared on mouse move. Keep TRANSFER for correctness.
    {
        struct virtio_gpu_transfer_to_host_2d xfer = {};
        xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        xfer.r.x = 0;
        xfer.r.y = 0;
        xfer.r.width = cur_.width;
        xfer.r.height = cur_.height;
        xfer.offset = 0;
        xfer.resource_id = fb_resource_id_;
        struct virtio_gpu_ctrl_hdr resp0;
        int r0 = virtio_gpu_submit(virtio_gpu_dev(), &xfer, sizeof(xfer),
                                   &resp0, sizeof(resp0), 1000);
        if (r0 < 0) {
            printk("virtio-gpu: transfer_to_host failed %d\n", r0);
            return false;
        }
    }

    struct virtio_gpu_resource_flush cmd = {};
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = cur_.width;
    cmd.r.height = cur_.height;
    cmd.resource_id = fb_resource_id_;

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

    struct virtio_gpu_device *vgpu = virtio_gpu_dev();
    (void)vgpu;
    // Previous 3D path reused host-visible GOP FB 2@0x80000000 to avoid
    // TRANSFER, but with sdl,gl + virgl that VRAM is not reliably scanned out
    // (host GL uses separate texture, direct VRAM writes tear/flicker and only
    // cursor rects appeared). Keep the proven 2D path even with 3D: allocate
    // a fresh RAM resource and use TRANSFER+FLUSH for every present. 3D ctx
    // stays alive for Mesa probes, scanout stays 2D-compatible.
    // (If you want the VRAM reuse, set QEMU_GPU_DEVICE=virtio-vga and skip virgl.)

    // Allocate DMA32 framebuffer (>1M, <4G) like Linux drm_gem_shmem + dma_alloc
    void *pages = pmm_alloc_frames_high(fb_pages_, 0x100000000ULL);
    if (!pages) pages = pmm_alloc_frames(fb_pages_);
    if ((uintptr_t)pages && (uintptr_t)pages < 0x100000) {
        void *pages2 = pmm_alloc_frames_high(fb_pages_, 0x100000000ULL);
        if (pages2 && (uintptr_t)pages2 >= 0x100000) {
            pmm_free_frames(pages, fb_pages_);
            pages = pages2;
        } else if (pages2) {
            pmm_free_frames(pages2, fb_pages_);
        }
    }
    if (!pages) {
        printk("virtio-gpu: failed to alloc backing pages\n");
        return false;
    }
    fb_phys_ = (uint64_t)(uintptr_t)pages;

    fb_ = vmm_mmap_phys(fb_phys_, fb_pages_, MMU_WRITE | MMU_UNCACHED);
    if (!fb_) {
        pmm_free_frames(pages, fb_pages_);
        fb_phys_ = 0;
        fb_pages_ = 0;
        printk("virtio-gpu: failed to map backing pages\n");
        return false;
    }
    memset(fb_, 0, fb_size_);

    /* Allocate a fresh resource id – avoid colliding with OVMF's 1/2 */
    if (fb_resource_id_ == 0) {
        fb_resource_id_ = virtio_gpu_alloc_resource_id();
        if (fb_resource_id_ < 10) fb_resource_id_ += 10;
        virtio_gpu_dev()->fb_resource_id = fb_resource_id_;
    }

    /* 1. RESOURCE_CREATE_2D – required before ATTACH_BACKING (virtgpu_vq.c:642). */
    {
        struct virtio_gpu_resource_create_2d cre = {};
        cre.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cre.resource_id = fb_resource_id_;
        cre.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM; /* X8R8G8B8 matches cur_.format */
        cre.width = cur_.width;
        cre.height = cur_.height;
        struct virtio_gpu_ctrl_hdr resp;
        int r = virtio_gpu_submit(virtio_gpu_dev(), &cre, sizeof(cre), &resp, sizeof(resp), 1000);
        if (r < 0) {
            printk("virtio-gpu: resource_create_2d %u %ux%u failed %d\n",
                   fb_resource_id_, cur_.width, cur_.height, r);
            vmm_unmap_page(fb_phys_);
            pmm_free_frames(pages, fb_pages_);
            fb_phys_ = 0; fb_pages_ = 0; fb_resource_id_ = 0;
            return false;
        }
        printk("virtio-gpu: created 2d resource %u %ux%u\n",
               fb_resource_id_, cur_.width, cur_.height);
    }

    /* 2. RESOURCE_ATTACH_BACKING – Linux per-page SG first (like virtio_gpu_object_shmem_init) */
    {
        // Try per-page SG first - QEMU virgl with DMA needs PAGE-aligned entries,
        // single large 4M entry with nr=1 may be rejected as INVALID_PARAMETER (0x1205)
        size_t cmd_size2 = sizeof(struct virtio_gpu_resource_attach_backing) +
                           fb_pages_ * sizeof(struct virtio_gpu_mem_entry);
        struct virtio_gpu_resource_attach_backing *cmd2 =
            (struct virtio_gpu_resource_attach_backing *)kmalloc(cmd_size2);
        if (!cmd2) {
            pmm_free_frames(pages, fb_pages_);
            fb_phys_ = 0; fb_pages_ = 0;
            return false;
        }
        memset(cmd2, 0, cmd_size2);
        cmd2->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        cmd2->resource_id = fb_resource_id_;
        cmd2->nr_entries = fb_pages_;
        struct virtio_gpu_mem_entry *entries2 =
            (struct virtio_gpu_mem_entry *)((uint8_t *)cmd2 + sizeof(*cmd2));
        for (uint32_t i = 0; i < fb_pages_; i++) {
            entries2[i].addr = fb_phys_ + (uint64_t)i * PAGE_SIZE;
            uint32_t len = PAGE_SIZE;
            if (i == fb_pages_ - 1) {
                uint32_t rem = fb_size_ % PAGE_SIZE;
                if (rem) len = rem;
            }
            entries2[i].length = len;
            entries2[i].padding = 0;
        }
        printk("virtio-gpu: attach per-page res %u fb_phys 0x%lx pages %u\n",
               fb_resource_id_, (unsigned long)fb_phys_, fb_pages_);
        struct virtio_gpu_ctrl_hdr resp;
        int r2 = virtio_gpu_submit(virtio_gpu_dev(), cmd2, cmd_size2,
                                    &resp, sizeof(resp), 2000);
        kfree(cmd2);
        if (r2 < 0) {
            printk("virtio-gpu: attach_backing per-page %u x%u failed %d (resp 0x%x), trying single\n",
                   fb_resource_id_, fb_pages_, r2, resp.type);
            // Fallback to single contiguous entry
            size_t cmd_size = sizeof(struct virtio_gpu_resource_attach_backing) +
                              sizeof(struct virtio_gpu_mem_entry);
            struct virtio_gpu_resource_attach_backing *cmd =
                (struct virtio_gpu_resource_attach_backing *)kmalloc(cmd_size);
            if (!cmd) {
                struct virtio_gpu_resource_unref unref = {};
                unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
                unref.resource_id = fb_resource_id_;
                struct virtio_gpu_ctrl_hdr resp2;
                virtio_gpu_submit(virtio_gpu_dev(), &unref, sizeof(unref), &resp2, sizeof(resp2), 1000);
                vmm_unmap_page(fb_phys_);
                pmm_free_frames(pages, fb_pages_);
                fb_phys_ = 0; fb_pages_ = 0; fb_resource_id_ = 0;
                return false;
            }
            memset(cmd, 0, cmd_size);
            cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
            cmd->resource_id = fb_resource_id_;
            cmd->nr_entries = 1;
            struct virtio_gpu_mem_entry *entries =
                (struct virtio_gpu_mem_entry *)((uint8_t *)cmd + sizeof(*cmd));
            entries[0].addr = fb_phys_;
            entries[0].length = fb_size_;
            entries[0].padding = 0;
            printk("virtio-gpu: attach single res %u fb_phys 0x%lx size %u\n",
                   fb_resource_id_, (unsigned long)fb_phys_, fb_size_);
            int r = virtio_gpu_submit(virtio_gpu_dev(), cmd, cmd_size,
                                        &resp, sizeof(resp), 2000);
            kfree(cmd);
            if (r < 0) {
                printk("virtio-gpu: attach_backing single %u x%u failed %d, falling back to GOP reuse\n",
                       fb_resource_id_, fb_pages_, r);
                struct virtio_gpu_resource_unref unref = {};
                unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
                unref.resource_id = fb_resource_id_;
                struct virtio_gpu_ctrl_hdr resp2;
                virtio_gpu_submit(virtio_gpu_dev(), &unref, sizeof(unref), &resp2, sizeof(resp2), 1000);
                vmm_unmap_page((uintptr_t)fb_);
                pmm_free_frames(pages, fb_pages_);
                // Fallback: reuse OVMF's existing scanout 0 resource 2 at GOP FB 0x80000000
                fb_resource_id_ = 2;
                fb_phys_ = 0x80000000;
                fb_size_ = cur_.width * cur_.height * 4;
                fb_pages_ = (fb_size_ + PAGE_SIZE - 1) >> PAGE_SHIFT;
                fb_ = vmm_mmap_phys(fb_phys_, fb_pages_, MMU_WRITE | MMU_UNCACHED);
                if (!fb_) fb_ = (void*)(uintptr_t)0x80000000;
                printk("virtio-gpu: reusing GOP FB %u at 0x%lx mapped %p\n",
                       fb_resource_id_, (unsigned long)fb_phys_, fb_);
                // Don't return false, continue to set scanout with existing res
            } else {
                printk("virtio-gpu: attached backing %u: %u pages phys 0x%lx size %u\n",
                       fb_resource_id_, fb_pages_, (unsigned long)fb_phys_, fb_size_);
            }
        } else {
            printk("virtio-gpu: attached backing %u: %u pages phys 0x%lx size %u\n",
                   fb_resource_id_, fb_pages_, (unsigned long)fb_phys_, fb_size_);
        }
    }
    return true;
}

bool VirtioGpuDisplay::setup_scanout(void)
{
    printk("virtio-gpu: setup_scanout entry res %u fb_phys 0x%lx %ux%u\n",
           fb_resource_id_, (unsigned long)fb_phys_, cur_.width, cur_.height);
    // If we are reusing the GOP FB (resource 2 at 0x80000000) that OVMF already
    // set as scanout, we don't need to do set_scanout again – it's already active.
    // The GOP display is at 1280x800 and OVMF already did set_scanout for it.
    if (fb_resource_id_ == 2 && fb_phys_ == 0x80000000) {
        printk("virtio-gpu: reusing GOP scanout 0 -> resource %u %ux%u (skip set_scanout)\n",
               fb_resource_id_, cur_.width, cur_.height);
        return true;
    }
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
        printk("virtio-gpu: set_scanout %u failed %d, trying to continue (may already be set)\n",
               fb_resource_id_, r);
        // Don't fail – the scanout may already be set by OVMF, and our
        // present_flush (xfer+flush) will still work for the existing res.
        // For the reused GOP case, we already handled above, but for other
        // cases, allow to continue and let the display be considered online.
        // Only fail if it's a fresh resource that we just created and it
        // really needs scanout.
        if (fb_resource_id_ != 2) return false;
        printk("virtio-gpu: scanout already set, continuing\n");
        return true;
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

    /* 3D: create a virgl context if host supports it (like virtgpu_drv.c:401) */
    if (d->has_virgl_3d && ctx_id_ == 0) {
        ctx_id_ = virtio_gpu_alloc_context_id();
        const char *name = "axiome";
        int r = virtio_gpu_create_context(ctx_id_, 0, name);
        if (r < 0) {
            printk("virtio-gpu: ctx_create %u failed %d (3D disabled)\n", ctx_id_, r);
            ctx_id_ = 0;
            d->has_virgl_3d = 0; /* fallback to 2D */
        } else {
            printk("virtio-gpu: 3D context %u created (%s)\n", ctx_id_, name);
            // Linux-like capset validation: check num_capsets from config (virtio_cread)
            // and validate id 1..63 and size. If host has no capsets or bogus, keep 2D
            // scanout but don't fail display – Mesa will probe via GET_CAPS later.
            if (d->num_capsets == 0) {
                printk("virtio-gpu: num_capsets==0 from config, host may not expose virgl capsets (QEMU without virglrenderer) - keeping 2D, 3D ctx stays\n");
            } else {
                struct virtio_gpu_get_capset_info cinfo = {};
                cinfo.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
                cinfo.capset_index = 0;
                struct virtio_gpu_resp_capset_info cinfo_resp;
                memset(&cinfo_resp, 0, sizeof(cinfo_resp));
                int cr = virtio_gpu_submit(d, &cinfo, sizeof(cinfo), &cinfo_resp, sizeof(cinfo_resp), 2000);
                if (cr == 0) {
                    printk("virtio-gpu: capset0 id=%u ver=%u size=%u (num_capsets=%u)\n",
                           cinfo_resp.capset_id, cinfo_resp.capset_max_version, cinfo_resp.capset_max_size, d->num_capsets);
                    if (cinfo_resp.capset_id == 0 || cinfo_resp.capset_id > 63 ||
                        cinfo_resp.capset_max_size == 0 || cinfo_resp.capset_max_size > 8*1024*1024) {
                        printk("virtio-gpu: capset0 bogus (expected virgl id 1..4 size ~70k) - host virgl missing, keeping 2D scanout\n");
                        // Don't destroy ctx, but mark has_virgl as still true for Mesa to try GET_CAPS
                        // If you want strict, set has_virgl=0 here to force 2D only.
                    } else if (cinfo_resp.capset_id > 4) {
                        printk("virtio-gpu: capset0 id %u not classic virgl (1..4) but valid, keeping 3D\n", cinfo_resp.capset_id);
                    }
                } else {
                    printk("virtio-gpu: capset0 query failed %d (num_capsets=%u, keeping 2D scanout)\n", cr, d->num_capsets);
                }
            }
        }
    }

    ready_ = true;
    printk("gfx: virtio-gpu online %ux%u pitch=%u 3D=%s\n",
           cur_.width, cur_.height, cur_.pitch, d->has_virgl_3d ? "yes" : "no");
    return true;
}

GfxCaps VirtioGpuDisplay::caps() const
{
    GfxCaps c;
    c.has_hw_fill = 1;
    c.has_hw_blit = 1;
    c.has_hw_flip = 0;
    c.has_3d = virtio_gpu_dev()->has_virgl_3d ? 1 : 0;
    c.has_alpha = 1;
    c.has_compositor = 1;
    c.has_shadows = 1;
    c.has_vsync = 0;
    c.has_hw_cursor = 0;
    c.has_scale = 1;
    c.has_yuv = 0;
    c.has_shaders = c.has_3d;
    c.has_blur = c.has_3d; // blur via shader only when 3D available; otherwise CPU-gated off
    // Rounded + gradient come for free with compositor
    c.bits = 0;
    gfx_caps_sync_bits(c);
    // Startup self-test: if 3D advertised but capset missing, mask blur/shaders
    // (kept optimistic; manager will keep bits as-is – mesa probe will fail later)
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
    // Don't flush mid-frame - compositor will call present() once per frame.
    // Flushing on every primitive causes QEMU GL to present intermediate
    // black/gui states => flicker (alternating 0x107/0x105 spam in logs).
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
}

void VirtioGpuDisplay::present()
{
    present_flush();
}

} /* namespace gfx */
