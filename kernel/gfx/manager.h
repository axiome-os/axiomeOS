#ifndef AXIOME_GFX_MANAGER_H
#define AXIOME_GFX_MANAGER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include "display.h"

namespace gfx {

/* Display manager: owns the backend registry and the active display.
   Backends are placement-new'd into static storage (no heap, no RTTI),
   registered once from gfx_init(), then the best probing backend wins.
   Native drivers that are still stubs probe true but fail init() until
   their modeset lands, so selection gracefully falls back to GOP. */
class DisplayManager {
public:
    static const int kMaxDisplays = 4;

    bool reg(IDisplay *d);
    void select_best();
    IDisplay *active() const { return active_; }
    const char *active_name() const;

    /* Properties – temporary kill-switch for drivers. Set before
        select_best()/gfx_init() to force GOP fallback. */
    GfxProperties &properties() { return props_; }
    const GfxProperties &properties() const { return props_; }
    void set_disable_drivers(bool v) { props_.disable_drivers = v; }
    void set_disable_mask(uint32_t mask) { props_.disable_mask = mask; }
    bool is_disabled(const char *name) const;

    /* Convenience fan-out used by the C API and the console. */
    void clear(uint32_t rgb);
    void fill_rect(const GfxRect &r, uint32_t rgb);
    void present();
    bool present_rect(const GfxRect &r);
    GfxCaps caps() const;
    uint64_t caps_bits() const;
    const char *detail_mode() const;
    void alpha_blend_rect(const GfxRect &r, uint32_t rgb, uint8_t alpha);
    void blur_rect(const GfxRect &r, uint32_t radius);
    void shadow_rect(const GfxRect &r, uint32_t blur, uint32_t color);

private:
    static uint32_t driver_flag_for_name(const char *name);

    IDisplay *slots_[kMaxDisplays] = {};
    int count_ = 0;
    IDisplay *active_ = nullptr;
    GfxProperties props_{};
};

DisplayManager &display_manager(void);

} /* namespace gfx */

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

/* C ABI for the C console, syscalls and driver glue. */
void gfx_init(void);
int gfx_active(void);
const char *gfx_active_name(void);
void gfx_clear(uint32_t rgb);
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t rgb);
void gfx_present(void);
int gfx_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int gfx_mode(uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp);
void *gfx_cpu_base(void);
/* Caps query (capability flags). Returns 0 on success. */
int gfx_caps(uint64_t *out_bits);
int gfx_caps_has(uint64_t cap);
const char *gfx_detail_mode(void);
/* Extended 2D / effects (CPU fallback if hw missing) */
void gfx_alpha_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t rgb, uint8_t alpha);
void gfx_blur_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t radius);
void gfx_shadow_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t blur, uint32_t color);

/* Property API – temporary in-code disable for drivers. Safe to call
    before gfx_init() (preferred) or at runtime before next select_best(). */
void gfx_set_disable_drivers(int disable);
void gfx_set_disable_mask(uint32_t mask);
uint32_t gfx_get_disable_mask(void);
int gfx_is_driver_disabled(const char *name);

#ifdef __cplusplus
}
#endif

#endif
