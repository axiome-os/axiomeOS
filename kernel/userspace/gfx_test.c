/* gfx_test: end-to-end exercise of /Devices/dri0 (kernel/dri.c).
   GET_MODE -> CREATE -> MMAP -> paint gradient -> PRESENT -> DESTROY.
   Any failure prints "dri: FAIL <stage>" and exits nonzero; success prints
   "dri: PASS" (the gradient should also be visible on screen). */
#include "stdio.h"
#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#define AXDRI_MAGIC 0x41584452u
#define AXDRI_CMD_SIZE 32

enum {
    AXDRI_GET_MODE = 0x01,
    AXDRI_DUMB_CREATE = 0x02,
    AXDRI_DUMB_MAP = 0x03,
    AXDRI_DUMB_DESTROY = 0x04,
    AXDRI_PRESENT = 0x05,
    AXDRI_GET_CAPS = 0x06,
};

struct axdri_caps {
    uint64_t caps;
    uint32_t detail;
    uint32_t pad;
};

struct axdri_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
};

struct axdri_cmd {
    uint32_t magic;
    uint32_t op;
    uint32_t args[6];
};

#define TW 256
#define TH 256
/* Dumb buffers are mmap'd here: PML4[4] (2TB) — under a USER PML4 entry and
   clear of the app image (PML4[2]), libc.sl (PML4[3]) and the supervisor
   low/half entries. See dri_mmap()'s window guard. */
#define MAP_ADDR ((void *)0x200000000000ULL)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int fd = open("/Devices/dri0", 2);
    if (fd < 0)
    {
        printf("dri: FAIL open\n");
        return 1;
    }

    struct axdri_mode m;
    if (read(fd, &m, sizeof(m)) != (long)sizeof(m) || m.width == 0)
    {
        printf("dri: FAIL get-mode\n");
        return 1;
    }
    printf("dri: mode %ux%u pitch=%u bpp=%u\n", m.width, m.height, m.pitch,
           m.bpp);
    /* Query caps */
    {
        struct axdri_cmd gc;
        gc.magic = AXDRI_MAGIC;
        gc.op = AXDRI_GET_CAPS;
        gc.args[0]=0;gc.args[1]=0;gc.args[2]=0;gc.args[3]=0;gc.args[4]=0;gc.args[5]=0;
        if (write(fd, &gc, sizeof(gc)) == (long)sizeof(gc)) {
            uint64_t caps = ((uint64_t)gc.args[1]<<32)|gc.args[0];
            const char *mode2 = gc.args[2] ? "detailed" : "simplified";
            printf("dri: caps 0x%lx mode=%s blur=%d alpha=%d compositor=%d shadows=%d shaders=%d 3d=%d\n",
                   (unsigned long)caps, mode2,
                   (caps>>6)&1, (caps>>4)&1, (caps>>5)&1, (caps>>7)&1, (caps>>8)&1, (caps>>3)&1);
            if (!(caps & (1ull<<6))) printf("dri: blur disabled on CPU – use simplified path\n");
        } else {
            struct axdri_caps caps2;
            if (read(fd, &caps2, sizeof(caps2)) == (long)sizeof(caps2))
                printf("dri: caps 0x%lx detail=%u via read\n", (unsigned long)caps2.caps, caps2.detail);
        }
    }

    struct axdri_cmd c;
    c.magic = AXDRI_MAGIC;
    c.op = AXDRI_DUMB_CREATE;
    c.args[0] = TW;
    c.args[1] = TH;
    c.args[2] = 0;
    c.args[3] = 0;
    c.args[4] = 0;
    c.args[5] = 0;
    if (write(fd, &c, sizeof(c)) != (long)sizeof(c) || c.args[3] == 0)
    {
        printf("dri: FAIL create\n");
        return 1;
    }
    uint32_t pitch = c.args[2];
    uint32_t handle = c.args[3];
    uint64_t size = ((uint64_t)c.args[5] << 32) | c.args[4];
    printf("dri: dumb handle=%u pitch=%u size=%u\n", handle, pitch,
           (unsigned)size);
    if (pitch != TW * 4 || size != (uint64_t)TW * TH * 4)
    {
        printf("dri: FAIL layout\n");
        return 1;
    }

    uint64_t off = ((uint64_t)handle << 32);
    long mr = syscall(SYS_MMAP, (long)fd, (long)off, (long)MAP_ADDR,
                      (long)size, 0, 0);
    if (mr != 0)
    {
        printf("dri: FAIL mmap\n");
        return 1;
    }

    volatile uint32_t *px = (volatile uint32_t *)MAP_ADDR;
    for (uint32_t y = 0; y < TH; y++)
        for (uint32_t x = 0; x < TW; x++)
            px[y * TW + x] = 0xFF000000u | (x << 16) | (y << 8) | (x ^ y);

    /* Readback through the same mapping (proves write visibility). */
    if (px[0] != 0xFF000000u)
    {
        printf("dri: FAIL readback\n");
        return 1;
    }

    uint32_t dx = 64, dy = 64;
    if (dx + TW > m.width || dy + TH > m.height)
    {
        dx = 0;
        dy = 0;
    }
    struct axdri_cmd p;
    p.magic = AXDRI_MAGIC;
    p.op = AXDRI_PRESENT;
    p.args[0] = handle;
    p.args[1] = (0u << 16) | 0u;
    p.args[2] = (dy << 16) | dx;
    p.args[3] = (TH << 16) | TW;
    p.args[4] = 0;
    p.args[5] = 0;
    if (write(fd, &p, sizeof(p)) != (long)sizeof(p))
    {
        printf("dri: FAIL present\n");
        return 1;
    }

    struct axdri_cmd k;
    k.magic = AXDRI_MAGIC;
    k.op = AXDRI_DUMB_DESTROY;
    k.args[0] = handle;
    k.args[1] = 0;
    k.args[2] = 0;
    k.args[3] = 0;
    k.args[4] = 0;
    k.args[5] = 0;
    if (write(fd, &k, sizeof(k)) != (long)sizeof(k))
    {
        printf("dri: FAIL destroy\n");
        return 1;
    }

    close(fd);
    printf("dri: PASS handle=%u %ux%u presented at %u,%u\n", handle, TW, TH,
           dx, dy);
    return 0;
}
