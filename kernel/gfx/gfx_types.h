#ifndef AXIOME_GFX_TYPES_H
#define AXIOME_GFX_TYPES_H

#include <stdint.h>
#include <stddef.h>

namespace gfx {

/* Pixel formats seen on real hardware. The UEFI GOP currently hands us
   X8R8G8B8 in either byte order; Intel native planes are the same layouts
   once display engine palette/gamma is bypassed. */
enum class PixelFormat : uint8_t {
    Unknown = 0,
    X8R8G8B8 = 1, /* R byte at lowest address (UEFI RedGreenBlueReserved) */
    X8B8G8R8 = 2, /* B byte first (UEFI BlueGreenRedReserved) */
};

inline uint32_t pixel_format_bpp(PixelFormat fmt)
{
    switch (fmt) {
        case PixelFormat::X8R8G8B8:
        case PixelFormat::X8B8G8R8:
            return 32;
        default:
            return 0;
    }
}

/* Convert a canonical 0x00RRGGBB colour to the physical byte order of `fmt`.
   The framebuffer fast paths store native-endian u32, so this is a channel
   swap, not an endian swap. */
inline uint32_t canonical_to_native(uint32_t rgb, PixelFormat fmt)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = (rgb >> 0) & 0xFF;
    if (fmt == PixelFormat::X8B8G8R8)
        return (b << 16) | (g << 8) | r;
    return (r << 16) | (g << 8) | b;
}

struct GfxMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0; /* bytes per scanline */
    PixelFormat format = PixelFormat::Unknown;
};

struct GfxRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

/* Clip `r` against a `width x height` surface. Returns false when fully
   off-screen (w/h set to 0). Header-only so host unit tests can include
   this file directly. */
inline bool clip_rect(GfxRect &r, uint32_t width, uint32_t height)
{
    if (r.w == 0 || r.h == 0)
        return false;
    if (r.x >= width || r.y >= height) {
        r.w = 0;
        r.h = 0;
        return false;
    }
    if (r.x + r.w > width)
        r.w = width - r.x;
    if (r.y + r.h > height)
        r.h = height - r.y;
    return r.w != 0 && r.h != 0;
}

/* Acceleration capabilities: what a backend can do without CPU pixel
    pushing. GOP provides none; Intel provides blit/fill once GTT + engine
    init lands. Mesa queries this to decide client vs GPU submission. */
struct GfxCaps {
    int has_hw_fill = 0;
    int has_hw_blit = 0;
    int has_hw_flip = 0; /* async page-flip / vsync */
    int has_3d = 0;      /* 3D engine available (i915/Xe) */
};

/* Per-driver disable flags (bitmask). GOP is never disabled – it's the
    fallback. Used by GfxProperties::disable_mask. */
enum GfxDriverFlag : uint32_t {
    GFX_DRIVER_VIRTIO = 1u << 0,
    GFX_DRIVER_BOCHS = 1u << 1,
    GFX_DRIVER_INTEL = 1u << 2,
    GFX_DRIVER_ALL = GFX_DRIVER_VIRTIO | GFX_DRIVER_BOCHS | GFX_DRIVER_INTEL,
};

/* Runtime knobs for the display manager. Temporary / bring-up helper:
    flip `disable_drivers` or `disable_mask` in code (or via the C API) to
    force-fallback to GOP without deleting driver objects.

    Examples (in C++ code, before gfx_init()):
      gfx::display_manager().properties().disable_drivers = true; // all off
      gfx::display_manager().properties().disable_mask = GFX_DRIVER_INTEL;
      gfx::display_manager().properties().disable_mask = GFX_DRIVER_VIRTIO | GFX_DRIVER_INTEL;
 */
struct GfxProperties {
    bool disable_drivers = false;  /* master kill-switch: only GOP remains */
    uint32_t disable_mask = 0;     /* per-driver mask when master is false */
};

} /* namespace gfx */

#endif
