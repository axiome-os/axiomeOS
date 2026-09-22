#ifndef AXIOME_GFX_MESA_ABI_H
#define AXIOME_GFX_MESA_ABI_H

/* Minimal DRM-style ABI for the Mesa bring-up.
   This is NOT a Linux DRM clone: it is the smallest surface Mesa's
   platform layer needs (dumb buffers + present) on top of IDisplay.
   Keep this header freestanding (stdint only) so both kernel C++ and the
   userspace Mesa platform patch can include it. */

#include <stdint.h>

#define AXDRI_MAGIC 0x41584452u /* "AXDR" */

enum axdri_ioctl {
    AXDRI_GET_MODE = 0x01,   /* out: axdri_mode */
    AXDRI_DUMB_CREATE = 0x02,/* in/out: axdri_dumb */
    AXDRI_DUMB_MAP = 0x03,   /* in: handle -> out: offset for mmap */
    AXDRI_DUMB_DESTROY = 0x04,
    AXDRI_PRESENT = 0x05,    /* blit dumb buffer to scanout + flush */
    AXDRI_GET_CAPS = 0x06,   /* out: axdri_caps (capability flags) */
};

struct axdri_caps {
    uint64_t caps;      /* GfxCap bitmask */
    uint32_t detail;    /* 0=simplified 1=detailed */
    uint32_t pad;
};

struct axdri_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  /* bytes per line */
    uint32_t bpp;    /* always 32 on GOP */
    uint32_t format; /* gfx::PixelFormat as u32 */
};

struct axdri_dumb {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  /* out */
    uint32_t handle; /* out */
    uint64_t size;   /* out */
};

struct axdri_present {
    uint32_t handle;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t w;
    uint32_t h;
};

#endif
