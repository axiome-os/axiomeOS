#ifndef AXIOME_AXDRI_CMD_H
#define AXIOME_AXDRI_CMD_H

/* Write-channel command protocol for /Devices/dri0.
   dev_ops has no ioctl slot, so Mesa's winsys speaks through write():
   one fixed-size command struct per call. GET_MODE is a plain read()
   returning struct axdri_mode (see gfx/mesa_abi.h).
   Freestanding (stdint only); included by kernel/dri.c AND host tests. */

#include <stdint.h>
#include "gfx/mesa_abi.h"

#define AXDRI_CMD_SIZE 32

struct axdri_cmd {
    uint32_t magic;   /* AXDRI_MAGIC */
    uint32_t op;      /* enum axdri_ioctl */
    uint32_t args[6]; /* op-specific inputs; DUMB_CREATE writes results back */
};

/* DUMB_CREATE args: [w, h] -> results written back into the SAME struct:
   args[2]=pitch, args[3]=handle, args[4]=size_lo, args[5]=size_hi.
   Works because sys_write passes the validated user buffer straight through
   and the page is user-writable (in-out ioctl-over-write). */
static inline void axdri_encode_create(struct axdri_cmd *c, uint32_t w,
                                       uint32_t h)
{
    c->magic = AXDRI_MAGIC;
    c->op = (uint32_t)AXDRI_DUMB_CREATE;
    c->args[0] = w;
    c->args[1] = h;
    c->args[2] = 0;
    c->args[3] = 0;
    c->args[4] = 0;
    c->args[5] = 0;
}

/* PRESENT args: [handle, src_x, src_y, dst_x, dst_y, w|h packed].
   w/h ride in args[5] as (h << 16) | w (both < 64k on every GOP mode). */
static inline void axdri_encode_present(struct axdri_cmd *c, uint32_t handle,
                                        uint32_t sx, uint32_t sy,
                                        uint32_t dx, uint32_t dy,
                                        uint32_t w, uint32_t h)
{
    c->magic = AXDRI_MAGIC;
    c->op = (uint32_t)AXDRI_PRESENT;
    c->args[0] = handle;
    c->args[1] = (sy << 16) | (sx & 0xFFFFu);
    c->args[2] = (dy << 16) | (dx & 0xFFFFu);
    c->args[3] = (h << 16) | (w & 0xFFFFu);
    c->args[4] = 0;
    c->args[5] = 0;
}

static inline void axdri_encode_destroy(struct axdri_cmd *c, uint32_t handle)
{
    c->magic = AXDRI_MAGIC;
    c->op = (uint32_t)AXDRI_DUMB_DESTROY;
    c->args[0] = handle;
    c->args[1] = 0;
    c->args[2] = 0;
    c->args[3] = 0;
    c->args[4] = 0;
    c->args[5] = 0;
}

static inline void axdri_encode_get_caps(struct axdri_cmd *c)
{
    c->magic = AXDRI_MAGIC;
    c->op = (uint32_t)AXDRI_GET_CAPS;
    c->args[0] = 0;
    c->args[1] = 0;
    c->args[2] = 0;
    c->args[3] = 0;
    c->args[4] = 0;
    c->args[5] = 0;
}

/* Pitch/size helpers shared by kernel/dri.c and the winsys (single source
   of truth so Mesa and the kernel can never disagree on layout). */
static inline uint32_t axdri_pitch_for(uint32_t w)
{
    return w * 4u;
}

static inline uint64_t axdri_size_for(uint32_t w, uint32_t h)
{
    return (uint64_t)axdri_pitch_for(w) * (uint64_t)h;
}

/* mmap offset encoding for dumb buffers: high 32 = handle, low 32 = byte
   offset inside the buffer (mirrors the SYS_MMAP fd+off calling convention
   already used by fb_mmap). */
static inline uint64_t axdri_mmap_off(uint32_t handle, uint32_t byte_off)
{
    return ((uint64_t)handle << 32) | (uint64_t)byte_off;
}

static inline uint32_t axdri_mmap_handle(uint64_t off)
{
    return (uint32_t)(off >> 32);
}

static inline uint32_t axdri_mmap_byte_off(uint64_t off)
{
    return (uint32_t)(off & 0xFFFFFFFFu);
}

#endif
