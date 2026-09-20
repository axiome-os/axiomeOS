#ifndef AXIOME_GFX_VIRTIO_GPU_DISPLAY_H
#define AXIOME_GFX_VIRTIO_GPU_DISPLAY_H

#include "display.h"

namespace gfx {

/* Virtio-gpu display backend.
 * Probes PCI 1AF4:1050, negotiates virtio transport, manages
 * scanouts through the virtio-gpu control vring. */
class VirtioGpuDisplay : public IDisplay {
public:
    VirtioGpuDisplay();
    const char *name() const override { return "virtio-gpu"; }
    int priority() const override { return 15; }
    bool probe() override;
    bool init(const GfxMode &mode) override;
    GfxMode mode() const override { return cur_; }
    GfxCaps caps() const override;
    void clear(uint32_t rgb) override;
    void fill_rect(const GfxRect &r, uint32_t rgb) override;
    void blit(const GfxRect &dst, const uint32_t *src,
               uint32_t src_pitch_px) override;
    void present() override;
    void *cpu_base() override { return fb_; }
    uint32_t fb_width() const override { return cur_.width; }
    uint32_t fb_height() const override { return cur_.height; }
    uint32_t fb_pitch() const override { return cur_.pitch; }
    PixelFormat fb_format() const override { return cur_.format; }

private:
    bool present_flush(void);
    bool create_framebuffer_resource(void);
    bool setup_scanout(void);

    bool ready_ = false;
    GfxMode cur_;
    void *fb_ = nullptr;
    uint64_t fb_phys_ = 0;
    uint32_t fb_pages_ = 0;
    uint32_t fb_size_ = 0;
    uint32_t fb_resource_id_ = 1;
};

VirtioGpuDisplay *virtio_gpu_display_create(void);

} /* namespace gfx */

#endif
