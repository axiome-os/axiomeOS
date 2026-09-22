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
    init lands. Mesa queries this to decide client vs GPU submission.

    Capability flags (bitmask): each bit is one testable feature. The
    `GfxCaps` struct keeps a `bits` field as the single source of truth;
    legacy `has_*` ints are kept for compat and auto-synced via helpers.
    GUI asks guixd -> gfx for caps and gates blur/shadows/shaders etc. */
enum GfxCap : uint64_t {
    GFX_CAP_HW_FILL    = 1ull << 0,
    GFX_CAP_HW_BLIT    = 1ull << 1,
    GFX_CAP_HW_FLIP    = 1ull << 2, /* async page-flip / vsync */
    GFX_CAP_3D         = 1ull << 3, /* 3D engine available (i915/Xe/virgl) */
    GFX_CAP_ALPHA      = 1ull << 4, /* per-pixel alpha blending */
    GFX_CAP_COMPOSITOR = 1ull << 5, /* compositor / layer blending */
    GFX_CAP_BLUR       = 1ull << 6, /* blur / backdrop-filter */
    GFX_CAP_SHADOWS    = 1ull << 7, /* drop shadows */
    GFX_CAP_SHADERS    = 1ull << 8, /* programmable shaders */
    GFX_CAP_VSYNC      = 1ull << 9, /* vsync / present sync */
    GFX_CAP_HW_CURSOR  = 1ull << 10,/* hardware cursor plane */
    GFX_CAP_SCALE      = 1ull << 11,/* hw scaling / stretch blit */
    GFX_CAP_YUV        = 1ull << 12,/* YUV overlay / video */
    GFX_CAP_GRADIENT   = 1ull << 13,/* hw gradient fill */
    GFX_CAP_ROUNDED    = 1ull << 14,/* hw rounded-rect / AA */
};

struct GfxCaps {
    uint64_t bits = 0;
    int has_hw_fill = 0;
    int has_hw_blit = 0;
    int has_hw_flip = 0; /* async page-flip / vsync */
    int has_3d = 0;      /* 3D engine available (i915/Xe) */
    /* extended (mirrors bits for convenience) */
    int has_alpha = 0;
    int has_compositor = 0;
    int has_blur = 0;
    int has_shadows = 0;
    int has_shaders = 0;
    int has_vsync = 0;
    int has_hw_cursor = 0;
    int has_scale = 0;
    int has_yuv = 0;
};

inline void gfx_caps_sync_bits(GfxCaps &c)
{
    c.bits = 0;
    if (c.has_hw_fill) c.bits |= GFX_CAP_HW_FILL;
    if (c.has_hw_blit) c.bits |= GFX_CAP_HW_BLIT;
    if (c.has_hw_flip) c.bits |= GFX_CAP_HW_FLIP;
    if (c.has_3d) c.bits |= GFX_CAP_3D;
    if (c.has_alpha) c.bits |= GFX_CAP_ALPHA;
    if (c.has_compositor) c.bits |= GFX_CAP_COMPOSITOR;
    if (c.has_blur) c.bits |= GFX_CAP_BLUR;
    if (c.has_shadows) c.bits |= GFX_CAP_SHADOWS;
    if (c.has_shaders) c.bits |= GFX_CAP_SHADERS;
    if (c.has_vsync) c.bits |= GFX_CAP_VSYNC;
    if (c.has_hw_cursor) c.bits |= GFX_CAP_HW_CURSOR;
    if (c.has_scale) c.bits |= GFX_CAP_SCALE;
    if (c.has_yuv) c.bits |= GFX_CAP_YUV;
    if (c.has_blur || c.has_shadows) c.bits |= GFX_CAP_GRADIENT; /* implicit */
}

inline void gfx_caps_sync_fields(GfxCaps &c)
{
    c.has_hw_fill    = (c.bits & GFX_CAP_HW_FILL) ? 1 : 0;
    c.has_hw_blit    = (c.bits & GFX_CAP_HW_BLIT) ? 1 : 0;
    c.has_hw_flip    = (c.bits & GFX_CAP_HW_FLIP) ? 1 : 0;
    c.has_3d         = (c.bits & GFX_CAP_3D) ? 1 : 0;
    c.has_alpha      = (c.bits & GFX_CAP_ALPHA) ? 1 : 0;
    c.has_compositor = (c.bits & GFX_CAP_COMPOSITOR) ? 1 : 0;
    c.has_blur       = (c.bits & GFX_CAP_BLUR) ? 1 : 0;
    c.has_shadows    = (c.bits & GFX_CAP_SHADOWS) ? 1 : 0;
    c.has_shaders    = (c.bits & GFX_CAP_SHADERS) ? 1 : 0;
    c.has_vsync      = (c.bits & GFX_CAP_VSYNC) ? 1 : 0;
    c.has_hw_cursor  = (c.bits & GFX_CAP_HW_CURSOR) ? 1 : 0;
    c.has_scale      = (c.bits & GFX_CAP_SCALE) ? 1 : 0;
    c.has_yuv        = (c.bits & GFX_CAP_YUV) ? 1 : 0;
}

inline bool gfx_caps_has(const GfxCaps &c, GfxCap cap)
{
    return (c.bits & (uint64_t)cap) != 0;
}

inline GfxCaps gfx_caps_from_bits(uint64_t bits)
{
    GfxCaps c;
    c.bits = bits;
    gfx_caps_sync_fields(c);
    return c;
}

/* Detail mode derived from caps: "simplified" when no accel, "detailed"
   when compositor/alpha/shadows available. Mirrors the GUI->guixd query. */
inline const char *gfx_detail_mode(const GfxCaps &c)
{
    if ((c.bits & (GFX_CAP_COMPOSITOR | GFX_CAP_ALPHA | GFX_CAP_BLUR |
                   GFX_CAP_SHADOWS | GFX_CAP_SHADERS)) != 0)
        return "detailed";
    if ((c.bits & (GFX_CAP_HW_FILL | GFX_CAP_HW_BLIT | GFX_CAP_3D)) != 0)
        return "detailed";
    return "simplified";
}

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
