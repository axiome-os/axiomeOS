/* /Devices/dri0 — minimal DRI device for the Mesa bring-up.
   Transport: read() returns struct axdri_mode; write() takes one struct
   axdri_cmd per call (ioctl-over-write; DUMB_CREATE writes pitch/handle/
   size back into the same user struct); mmap() maps a dumb buffer with the
   handle-in-high-32 off encoding from axdri_cmd.h.
   Backing store is pmm frames (identity-mapped, like kernel stacks);
   PRESENT blits CPU-side into the active gfx display and flushes. */

#include "driver.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "axdri_cmd.h"
#include "gfx/manager.h"
#include "framebuffer.h"
#include <stddef.h>
#include <stdint.h>

#define DRI_MAX_BUFS 16
#define DRI_MAX_DIM  8192

struct dri_buf {
    int used;
    uint64_t phys; /* identity-mapped; kernel virts == phys (see pmm/sched) */
    uint64_t pages;
    uint32_t w;
    uint32_t h;
    uint32_t pitch;
};

static struct dri_buf g_bufs[DRI_MAX_BUFS];

static struct dri_buf *dri_find(uint32_t handle)
{
    if (handle == 0 || handle > DRI_MAX_BUFS)
        return 0;
    struct dri_buf *b = &g_bufs[handle - 1];
    return b->used ? b : 0;
}

static uint32_t dri_alloc_slot(void)
{
    for (uint32_t i = 0; i < DRI_MAX_BUFS; i++)
        if (!g_bufs[i].used)
            return i + 1;
    return 0;
}

static long dri_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d;
    (void)off;
    /* GET_MODE: plain read of the active scanout mode. */
    if (len < sizeof(struct axdri_mode))
        return -1;
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
    if (gfx_mode(&w, &h, &pitch, &bpp) < 0)
        return -1;
    struct axdri_mode m;
    m.width = w;
    m.height = h;
    m.pitch = pitch;
    m.bpp = bpp;
    /* GOP-only for now: 32bpp X8R8G8B8 (format 1). */
    m.format = (bpp == 32) ? 1u : 0u;
    __builtin_memcpy(buf, &m, sizeof(m));
    return (long)sizeof(m);
}

static long dri_cmd_create(struct axdri_cmd *c)
{
    uint32_t w = c->args[0];
    uint32_t h = c->args[1];
    if (w == 0 || h == 0 || w > DRI_MAX_DIM || h > DRI_MAX_DIM)
        return -1;
    uint32_t handle = dri_alloc_slot();
    if (!handle)
        return -1;
    uint32_t pitch = axdri_pitch_for(w);
    uint64_t size = axdri_size_for(w, h);
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    void *phys = pmm_alloc_frames(pages);
    if (!phys)
        return -1;
    __builtin_memset((void *)(uintptr_t)phys, 0, (size_t)size);
    struct dri_buf *b = &g_bufs[handle - 1];
    b->used = 1;
    b->phys = (uint64_t)(uintptr_t)phys;
    b->pages = pages;
    b->w = w;
    b->h = h;
    b->pitch = pitch;
    /* In-out reply, written back into the caller's command struct. */
    c->args[2] = pitch;
    c->args[3] = handle;
    c->args[4] = (uint32_t)(size & 0xFFFFFFFFu);
    c->args[5] = (uint32_t)(size >> 32);
    return (long)AXDRI_CMD_SIZE;
}

static long dri_cmd_destroy(struct axdri_cmd *c)
{
    struct dri_buf *b = dri_find(c->args[0]);
    if (!b)
        return -1;
    pmm_free_frames((void *)(uintptr_t)b->phys, b->pages);
    __builtin_memset(b, 0, sizeof(*b));
    return (long)AXDRI_CMD_SIZE;
}

static long dri_cmd_present(struct axdri_cmd *c)
{
    struct dri_buf *b = dri_find(c->args[0]);
    if (!b)
        return -1;
    uint32_t sx = c->args[1] & 0xFFFFu;
    uint32_t sy = (c->args[1] >> 16) & 0xFFFFu;
    uint32_t dx = c->args[2] & 0xFFFFu;
    uint32_t dy = (c->args[2] >> 16) & 0xFFFFu;
    uint32_t w = c->args[3] & 0xFFFFu;
    uint32_t h = (c->args[3] >> 16) & 0xFFFFu;
    if (w == 0 || h == 0)
        return -1;

    uint32_t sw = 0, sh = 0, spitch = 0, sbpp = 0;
    if (gfx_mode(&sw, &sh, &spitch, &sbpp) < 0)
        return -1;
    uint8_t *scan = (uint8_t *)gfx_cpu_base();
    const uint8_t *src = (const uint8_t *)(uintptr_t)b->phys;
    if (!scan || !src)
        return -1;

    /* Clip against both source and scanout. */
    if (sx >= b->w || sy >= b->h || dx >= sw || dy >= sh)
        return -1;
    if (sx + w > b->w)
        w = b->w - sx;
    if (sy + h > b->h)
        h = b->h - sy;
    if (dx + w > sw)
        w = sw - dx;
    if (dy + h > sh)
        h = sh - dy;
    if (w == 0 || h == 0)
        return -1;

    for (uint32_t y = 0; y < h; y++)
    {
        const uint32_t *srow =
            (const uint32_t *)(src + (size_t)(sy + y) * b->pitch +
                               (size_t)sx * 4);
        uint32_t *drow = (uint32_t *)(scan + (size_t)(dy + y) * spitch +
                                      (size_t)dx * 4);
        for (uint32_t x = 0; x < w; x++)
            drow[x] = srow[x];
    }
    /* Direct staging-buffer write: dirty the rect so the flush below copies
       it to scanout (without this PRESENT is silently invisible). */
    fb_mark_dirty(dx, dy, w, h);
    gfx_present();
    return (long)AXDRI_CMD_SIZE;
}

static long dri_write(struct device *d, uint64_t off, const void *buf,
                      size_t len)
{
    (void)d;
    (void)off;
    if (len < sizeof(struct axdri_cmd))
        return -1;
    /* Local copy: the user buffer is validated by sys_write; work on the
       stack copy and write the reply back for DUMB_CREATE. */
    struct axdri_cmd c;
    __builtin_memcpy(&c, buf, sizeof(c));
    if (c.magic != AXDRI_MAGIC)
        return -1;
    switch (c.op)
    {
        case AXDRI_DUMB_CREATE:
        {
            long r = dri_cmd_create(&c);
            if (r < 0)
                return -1;
            __builtin_memcpy((void *)buf, &c, sizeof(c));
            return r;
        }
        case AXDRI_DUMB_DESTROY:
            return dri_cmd_destroy(&c);
        case AXDRI_PRESENT:
            return dri_cmd_present(&c);
        case AXDRI_DUMB_MAP: /* covered by the mmap off encoding; no-op */
        case AXDRI_GET_MODE: /* use read() */
        default:
            return -1;
    }
}

static long dri_mmap(struct device *d, uint64_t off, uint64_t virt,
                     size_t len, uint64_t flags)
{
    (void)d;
    (void)flags;
    struct dri_buf *b = dri_find(axdri_mmap_handle(off));
    if (!b)
        return -1;
    /* User mappings must sit under a USER PML4 entry. PML4[0] (identity),
       PML4[1] (mmu.c window), PML4[508] (vmm reserves) and PML4[511]
       (higher-half alias) are supervisor and shared into every address
       space; installing a user PTE beneath one faults with a protection
       violation (and would scribble on the shared kernel tables). The
       ELF loader already respects this (images at 0x8000000000+); enforce
       the same rule here with a clear error instead of a fault. */
    uint64_t pml4 = (virt >> 39) & 0x1FF;
    if (pml4 < 2 || pml4 > 507 || pml4 == 508)
    {
        printk("DRI: mmap virt 0x%lx outside user window\n", virt);
        return -1;
    }
    uint32_t byte_off = axdri_mmap_byte_off(off);
    uint64_t size = (uint64_t)b->pitch * b->h;
    if ((uint64_t)byte_off >= size)
        return -1;
    if (len > size - byte_off)
        return -1;
    struct thread *t = sched_current();
    if (!t || !t->mmu)
        return -1;
    uint64_t first = (byte_off >> PAGE_SHIFT);
    uint64_t skip = byte_off & (PAGE_SIZE - 1);
    size_t pages = (skip + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (first + pages > b->pages)
        return -1;
    for (size_t i = 0; i < pages; i++)
    {
        uint64_t pa = b->phys + (first + i) * PAGE_SIZE;
        if (vmm_map_page_in(t->mmu, virt + i * PAGE_SIZE, pa,
                            MMU_USER | MMU_WRITE) < 0)
            return -1;
    }
    return 0;
}

static struct dev_ops dri_ops = {
    .read = dri_read,
    .write = dri_write,
    .mmap = dri_mmap,
};

void dri_init(void)
{
    if (device_find("dri0"))
        return;
    struct device *d = (struct device *)kmalloc(sizeof(struct device));
    if (!d)
        return;
    __builtin_memset(d, 0, sizeof(*d));
    const char *n = "dri0";
    for (int i = 0; n[i] && i < 31; i++)
        d->name[i] = n[i];
    d->major = 226;
    d->minor = 0;
    d->type = DEV_CHAR;
    d->ops = dri_ops;
    device_register(d);
    printk("DRV: dri0 -> /Devices/dri0 (softpipe target)\n");
}
