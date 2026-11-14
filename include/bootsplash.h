#ifndef AXIOME_BOOTSPLASH_H
#define AXIOME_BOOTSPLASH_H

#include <stdint.h>
#include "bootsplash_assets.h"

struct splash_surface {
    uint8_t  *pixels;  /* 32-bit pixels (GOP and the kernel back-buffer) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
};

struct splash_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

static inline void splash_blend(struct splash_surface *s, int32_t x, int32_t y,
                                uint32_t color, uint32_t alpha)
{
    if (!s || !s->pixels || x < 0 || y < 0 ||
        (uint32_t)x >= s->width || (uint32_t)y >= s->height || alpha == 0)
        return;

    uint32_t *p = (uint32_t *)(s->pixels + (uint32_t)y * s->pitch +
                               (uint32_t)x * 4u);
    if (alpha >= 255) {
        *p = color;
        return;
    }

    uint32_t dst = *p;
    uint32_t inv = 255u - alpha;
    uint32_t r = (((color >> 16) & 0xFFu) * alpha +
                  ((dst >> 16) & 0xFFu) * inv) / 255u;
    uint32_t g = (((color >> 8) & 0xFFu) * alpha +
                  ((dst >> 8) & 0xFFu) * inv) / 255u;
    uint32_t b = ((color & 0xFFu) * alpha +
                  (dst & 0xFFu) * inv) / 255u;
    *p = (r << 16) | (g << 8) | b;
}

static inline void splash_clear(struct splash_surface *s, int32_t x, int32_t y,
                                int32_t w, int32_t h)
{
    if (!s || !s->pixels || w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)s->width)
        w = (int32_t)s->width - x;
    if (y + h > (int32_t)s->height)
        h = (int32_t)s->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int32_t row = 0; row < h; row++) {
        uint32_t *p = (uint32_t *)(s->pixels + (uint32_t)(y + row) * s->pitch +
                                   (uint32_t)x * 4u);
        for (int32_t col = 0; col < w; col++)
            p[col] = 0;
    }
}

/* Bilinear sample of an 8-bit coverage mask. */
static inline uint32_t splash_sample(const unsigned char *mask,
                                     uint32_t sw, uint32_t sh,
                                     uint32_t dx, uint32_t dw,
                                     uint32_t dy, uint32_t dh)
{
    uint64_t fx = dw > 1
        ? ((uint64_t)dx * (uint64_t)(sw - 1) << 16) / (dw - 1) : 0;
    uint64_t fy = dh > 1
        ? ((uint64_t)dy * (uint64_t)(sh - 1) << 16) / (dh - 1) : 0;

    uint32_t x0 = (uint32_t)(fx >> 16);
    uint32_t y0 = (uint32_t)(fy >> 16);
    uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
    uint32_t y1 = y0 + 1 < sh ? y0 + 1 : y0;
    uint32_t tx = (uint32_t)(fx & 0xFFFFu);
    uint32_t ty = (uint32_t)(fy & 0xFFFFu);

    uint32_t a = mask[y0 * sw + x0];
    uint32_t b = mask[y0 * sw + x1];
    uint32_t c = mask[y1 * sw + x0];
    uint32_t d = mask[y1 * sw + x1];

    uint32_t top = (a * (65536u - tx) + b * tx) >> 16;
    uint32_t bot = (c * (65536u - tx) + d * tx) >> 16;
    return (top * (65536u - ty) + bot * ty) >> 16;
}

static inline void splash_draw_logo(struct splash_surface *s,
                                    int32_t x, int32_t y,
                                    int32_t w, int32_t h)
{
    if (!s || !s->pixels || w <= 0 || h <= 0)
        return;

    for (int32_t oy = 0; oy < h; oy++) {
        for (int32_t ox = 0; ox < w; ox++) {
            uint32_t cov = splash_sample(splash_logo_mask, SPLASH_LOGO_W,
                                         SPLASH_LOGO_H, (uint32_t)ox,
                                         (uint32_t)w, (uint32_t)oy,
                                         (uint32_t)h);
            if (cov)
                splash_blend(s, x + ox, y + oy, 0x00FFFFFFu, cov);
        }
    }
}

static inline uint32_t splash_capsule_coverage(int32_t px, int32_t py,
                                               int32_t x, int32_t y,
                                               int32_t w, int32_t h)
{
    const int32_t ss = 4;
    int32_t r = (w < h ? w : h) / 2;

    if (r <= 0) {
        /* Degenerate one-pixel bar: fall back to a plain rectangle. */
        return (px >= x && px < x + w && py >= y && py < y + h) ? 255u : 0u;
    }

    /* Work in units of 1/(2*ss) pixel so every sample coordinate is an
       integer.  A pixel spans 2*ss units and its centre sits at px*2*ss. */
    int64_t x0 = (int64_t)(x + r) * 2 * ss;
    int64_t x1 = (int64_t)(x + w - r) * 2 * ss;
    int64_t cy = (int64_t)(2 * y + h) * ss;
    int64_t rad = (int64_t)r * 2 * ss;
    int hits = 0;

    for (int32_t sy = 0; sy < ss; sy++) {
        int64_t qy = (int64_t)py * 2 * ss + 2 * sy + 1 - cy;
        for (int32_t sx = 0; sx < ss; sx++) {
            int64_t qx = (int64_t)px * 2 * ss + 2 * sx + 1;
            int64_t dx = 0;

            if (qx < x0)
                dx = x0 - qx;
            else if (qx > x1)
                dx = qx - x1;
            if (dx * dx + qy * qy <= rad * rad)
                hits++;
        }
    }

    return (uint32_t)(hits * 255) / (uint32_t)(ss * ss);
}

static inline void splash_fill_capsule(struct splash_surface *s,
                                       int32_t x, int32_t y,
                                       int32_t w, int32_t h, uint32_t color)
{
    if (!s || !s->pixels || w <= 0 || h <= 0)
        return;

    for (int32_t oy = 0; oy < h; oy++) {
        for (int32_t ox = 0; ox < w; ox++) {
            uint32_t cov = splash_capsule_coverage(x + ox, y + oy,
                                                   x, y, w, h);
            if (cov)
                splash_blend(s, x + ox, y + oy, color, cov);
        }
    }
}

struct splash_geometry {
    int32_t logo_w;
    int32_t logo_h;
    int32_t logo_x;
    int32_t logo_y;
    int32_t bar_w;
    int32_t bar_h;
    int32_t bar_x;
    int32_t bar_y;
    struct splash_rect box;
};

static inline struct splash_geometry splash_geometry(struct splash_surface *s)
{
    struct splash_geometry g;
    uint32_t min_dim = s->width < s->height ? s->width : s->height;

    g.logo_w = (int32_t)(min_dim * 38u / 100u);
    if (g.logo_w < 120)
        g.logo_w = 120;
    if (g.logo_w > 560)
        g.logo_w = 560;
    g.logo_h = g.logo_w * SPLASH_LOGO_H / SPLASH_LOGO_W;

    g.bar_w = g.logo_w * 4 / 5;
    g.bar_h = g.logo_w * 3 / 100;
    if (g.bar_h < 4)
        g.bar_h = 4;
    if (g.bar_h > 12)
        g.bar_h = 12;

    /* Keep a clear gap so the bar sits noticeably below the logo. */
    int32_t gap = g.logo_h / 3;
    if (gap < g.bar_h * 3)
        gap = g.bar_h * 3;
    int32_t total_h = g.logo_h + gap + g.bar_h;
    int32_t top = ((int32_t)s->height - total_h) / 2;

    g.logo_x = ((int32_t)s->width - g.logo_w) / 2;
    g.logo_y = top;
    g.bar_x = ((int32_t)s->width - g.bar_w) / 2;
    g.bar_y = top + g.logo_h + gap;

    g.box.x = g.bar_x - 4;
    g.box.y = g.logo_y - 4;
    g.box.w = g.bar_w + 8;
    g.box.h = g.bar_y + g.bar_h - g.logo_y + 8;
    return g;
}

static inline struct splash_rect splash_render(struct splash_surface *s,
                                               uint32_t progress, int full)
{
    struct splash_geometry g = splash_geometry(s);
    struct splash_rect r;

    if (progress > 1000u)
        progress = 1000u;

    if (full) {
        r.x = 0;
        r.y = 0;
        r.w = (int32_t)s->width;
        r.h = (int32_t)s->height;
    } else {
        r = g.box;
    }
    splash_clear(s, r.x, r.y, r.w, r.h);

    splash_draw_logo(s, g.logo_x, g.logo_y, g.logo_w, g.logo_h);

    splash_fill_capsule(s, g.bar_x, g.bar_y, g.bar_w, g.bar_h, 0x00383838u);
    if (progress > 0) {
        int32_t fill_w = g.bar_w * (int32_t)progress / 1000;
        if (fill_w < g.bar_h)
            fill_w = g.bar_h;
        splash_fill_capsule(s, g.bar_x, g.bar_y, fill_w, g.bar_h,
                            0x00E8E8E8u);
    }
    return r;
}

#endif /* AXIOME_BOOTSPLASH_H */
