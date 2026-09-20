#ifndef AXIOME_GFX_BOCHS_DISPLAY_H
#define AXIOME_GFX_BOCHS_DISPLAY_H

#include "display.h"

namespace gfx {

/* QEMU std/VGA (Bochs VBE, PCI 1234:1111).
    Same staging story as IntelDisplay: probe now, delegate scanout to GOP
    until a native modeset lands. Priority sits between GOP and Intel so a
    future Bochs modeset beats GOP on QEMU but still loses to real Intel HW
    when both are present. */
class BochsDisplay : public IDisplay {
public:
    BochsDisplay();
    const char *name() const override { return "bochs"; }
    int priority() const override { return 20; }
    bool probe() override;
    bool init(const GfxMode &mode) override;
    GfxMode mode() const override { return shadow_; }
    GfxCaps caps() const override;
    void clear(uint32_t rgb) override;
    void fill_rect(const GfxRect &r, uint32_t rgb) override;
    void blit(const GfxRect &dst, const uint32_t *src,
               uint32_t src_pitch_px) override;
    void present() override;
    void *cpu_base() override { return nullptr; }
    uint32_t fb_width() const override { return shadow_.width; }
    uint32_t fb_height() const override { return shadow_.height; }
    uint32_t fb_pitch() const override { return shadow_.pitch; }
    PixelFormat fb_format() const override { return shadow_.format; }

private:
    bool found_ = false;
    GfxMode shadow_;
};

BochsDisplay *bochs_display_create(void);

} /* namespace gfx */

#endif
