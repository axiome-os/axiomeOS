#include "gop_display.h"
#include "../hal/hal.h"
extern "C" {
#include "../framebuffer.h"
#include "../hal/hal_bootinfo.h"
}

namespace gfx {

GopDisplay::GopDisplay() = default;

GopDisplay *gop_display_create(void)
{
    static unsigned char storage[sizeof(GopDisplay)];
    return new (storage) GopDisplay();
}

bool GopDisplay::probe()
{
    struct hal_bootinfo *bi = hal_bootinfo();
    return bi && bi->framebuffer_present && bi->fb_width > 0 &&
           bi->fb_height > 0;
}

bool GopDisplay::init(const GfxMode &mode)
{
    /* GOP owns its mode: firmware picked it in bootloader/gop.c. Accept the
       requested mode only when it matches the live framebuffer (or when the
       caller passes zeros = "keep current"). */
    struct hal_bootinfo *bi = hal_bootinfo();
    if (!bi || !bi->framebuffer_present)
        return false;
    if (mode.width != 0 && mode.width != bi->fb_width)
        return false;
    if (mode.height != 0 && mode.height != bi->fb_height)
        return false;
    cur_.width = bi->fb_width;
    cur_.height = bi->fb_height;
    cur_.pitch = bi->fb_pitch;
    /* hal_bootinfo only carries bpp; axboot pixel format was consumed by
       fb_init(). Both orders are 32bpp so report X8R8G8B8; native-order
       conversion lives in fill/blit via canonical_to_native when the
       firmware format string becomes available here. */
    cur_.format = PixelFormat::X8R8G8B8;
    ready_ = fb_active() != 0;
    return ready_;
}

GfxCaps GopDisplay::caps() const
{
    GfxCaps c;
    // GOP: no acceleration – simplified mode. All bits 0.
    // Explicitly sync to keep bits consistent.
    gfx_caps_sync_bits(c);
    return c;
}

void GopDisplay::clear(uint32_t rgb)
{
    if (!ready_)
        return;
    uint32_t fg, bg;
    fb_get_color(&fg, &bg);
    fb_set_color(fg, canonical_to_native(rgb, cur_.format));
    fb_clear();
    fb_set_color(fg, bg);
}

void GopDisplay::fill_rect(const GfxRect &r, uint32_t rgb)
{
    if (!ready_)
        return;
    GfxRect c = r;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    /* framebuffer.c has no rect primitive yet: write the clipped rows into
       the CPU staging buffer, then flush once (single WC burst). */
    uint32_t native = canonical_to_native(rgb, cur_.format);
    uint8_t *px = (uint8_t *)cpu_base();
    if (!px)
        return;
    uint32_t bpp = 4; /* GOP is always 32bpp; see bootloader/gop.c */
    for (uint32_t y = c.y; y < c.y + c.h; y++) {
        uint32_t *row =
            (uint32_t *)(px + (size_t)y * cur_.pitch + (size_t)c.x * bpp);
        for (uint32_t x = 0; x < c.w; x++)
            row[x] = native;
    }
    /* The pixels above landed directly in the staging buffer, bypassing the
       console's dirty tracking: mark the rect so present() flushes it. */
    fb_mark_dirty(c.x, c.y, c.w, c.h);
    fb_flush();
    /* Mark via a 1px glyph? No: framebuffer.c owns dirty tracking. The
       flush below covers it because we dirty the whole rect through the
       back-buffer path is internal. Simplest correct step: full present.
       A future fb_fill_rect() hook will narrow the dirty rect. */
    fb_flush();
    __asm__ volatile("sfence" ::: "memory");
}

void GopDisplay::blit(const GfxRect &dst, const uint32_t *src,
                      uint32_t src_pitch_px)
{
    if (!ready_ || !src)
        return;
    GfxRect c = dst;
    if (!clip_rect(c, cur_.width, cur_.height))
        return;
    uint8_t *px = (uint8_t *)cpu_base();
    if (!px)
        return;
    /* Account for clipping offset into the source. */
    uint32_t sx = c.x - dst.x;
    uint32_t sy = c.y - dst.y;
    for (uint32_t y = 0; y < c.h; y++) {
        const uint32_t *srow = src + (size_t)(sy + y) * src_pitch_px + sx;
        uint32_t *drow = (uint32_t *)(px + (size_t)(c.y + y) * cur_.pitch +
                                      (size_t)c.x * 4);
        for (uint32_t x = 0; x < c.w; x++)
            drow[x] = canonical_to_native(srow[x], cur_.format);
    }
    fb_mark_dirty(c.x, c.y, c.w, c.h);
    fb_flush();
    __asm__ volatile("sfence" ::: "memory");
}

void GopDisplay::present()
{
    if (!ready_)
        return;
    fb_flush();
}

void *GopDisplay::cpu_base()
{
    /* Back-buffer preferred (what fb_flush copies from); fall back to the
       WC scanout mapping when buffers are not up yet. */
    uint8_t *bb = fb_back_buffer_for_gfx();
    if (bb)
        return bb;
    return (void *)fb_addr();
}

} /* namespace gfx */
