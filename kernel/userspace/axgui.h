/* axgui.h — shared header-only toolkit for axiomeOS GUI programs.
   Included by axwm.c (mini window manager) and axterm.c (terminal).
   Single source of truth where possible: input ABI + 8x16 font come from
   kernel headers via relative includes; the DRI wire structs mirror
   kernel/axdri_cmd.h exactly (same pattern as gfx_test.c, which cannot see
   kernel headers under userspace -I rules).

   Transport = Mesa winsys transport: /Devices/dri0 dumb buffers (CREATE +
   SYS_MMAP + PRESENT). Software rasterization here is what Gallium softpipe
   will drop into (see ports/mesa-axiome/WINSYS.md). */

#ifndef AXGUI_H
#define AXGUI_H

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include <stddef.h>
#include <stdint.h>

/* Single source of truth with the kernel (kernel/input_abi.h). */
#include "../input_abi.h"
/* Compositor <-> client protocol (kernel/wm_abi.h). */
#include "../wm_abi.h"
/* 8x16 console font (kernel/font8x16.h). */
#include "../font8x16.h"

/* ---- DRI wire protocol (mirrors kernel/axdri_cmd.h) ---- */
#define AXGUI_DRI_MAGIC 0x41584452u
#define AXGUI_DRI_CMD_SIZE 32
#define AXGUI_MAP_HINT ((void *)0x200000000000ULL)

enum {
    AXGUI_GET_MODE = 0x01,
    AXGUI_DUMB_CREATE = 0x02,
    AXGUI_DUMB_MAP = 0x03,
    AXGUI_DUMB_DESTROY = 0x04,
    AXGUI_PRESENT = 0x05,
};

struct axgui_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
};

struct axgui_cmd {
    uint32_t magic;
    uint32_t op;
    uint32_t args[6];
};

struct axgui_fb {
    int fd;
    uint32_t w;
    uint32_t h;
    uint32_t pitch;
    uint32_t handle;
    uint64_t size;
    uint32_t *px;
};

static int axgui_dri_open(struct axgui_fb *fb)
{
    struct axgui_mode m;
    struct axgui_cmd c;
    if (!fb)
        return -1;
    fb->fd = open("/Devices/dri0", 2);
    if (fb->fd < 0)
        return -1;
    if (read(fb->fd, &m, sizeof(m)) != (long)sizeof(m) || m.width == 0)
    {
        close(fb->fd);
        return -1;
    }
    fb->w = m.width;
    fb->h = m.height;
    c.magic = AXGUI_DRI_MAGIC;
    c.op = AXGUI_DUMB_CREATE;
    c.args[0] = m.width;
    c.args[1] = m.height;
    c.args[2] = 0;
    c.args[3] = 0;
    c.args[4] = 0;
    c.args[5] = 0;
    if (write(fb->fd, &c, sizeof(c)) != (long)sizeof(c) || c.args[3] == 0)
    {
        close(fb->fd);
        return -1;
    }
    fb->pitch = c.args[2];
    fb->handle = c.args[3];
    fb->size = ((uint64_t)c.args[5] << 32) | c.args[4];
    if (fb->pitch != m.width * 4 ||
        fb->size != (uint64_t)m.width * m.height * 4)
    {
        close(fb->fd);
        return -1;
    }
    {
        uint64_t off = ((uint64_t)fb->handle << 32);
        long mr = syscall(SYS_MMAP, (long)fb->fd, (long)off,
                          (long)AXGUI_MAP_HINT, (long)fb->size, 0, 0);
        if (mr != 0)
        {
            close(fb->fd);
            return -1;
        }
    }
    fb->px = (uint32_t *)AXGUI_MAP_HINT;
    return 0;
}

static int axgui_present(struct axgui_fb *fb)
{
    struct axgui_cmd p;
    uint32_t w, h;
    if (!fb || fb->fd < 0)
        return -1;
    /* PRESENT packs geometry in 16 bits; clamp huge modes per call. */
    w = fb->w > 0xFFFFu ? 0xFFFFu : fb->w;
    h = fb->h > 0xFFFFu ? 0xFFFFu : fb->h;
    p.magic = AXGUI_DRI_MAGIC;
    p.op = AXGUI_PRESENT;
    p.args[0] = fb->handle;
    p.args[1] = 0;
    p.args[2] = 0;
    p.args[3] = (h << 16) | w;
    p.args[4] = 0;
    p.args[5] = 0;
    return (write(fb->fd, &p, sizeof(p)) == (long)sizeof(p)) ? 0 : -1;
}

static void axgui_close(struct axgui_fb *fb)
{
    struct axgui_cmd k;
    if (!fb || fb->fd < 0)
        return;
    k.magic = AXGUI_DRI_MAGIC;
    k.op = AXGUI_DUMB_DESTROY;
    k.args[0] = fb->handle;
    k.args[1] = 0;
    k.args[2] = 0;
    k.args[3] = 0;
    k.args[4] = 0;
    k.args[5] = 0;
    write(fb->fd, &k, sizeof(k));
    close(fb->fd);
    fb->fd = -1;
}

/* ---- input ---- */
static int axgui_input_open(void)
{
    return open("/Devices/input0", 0);
}

static void axgui_grab(int fd, int on)
{
    uint32_t v;
    if (fd < 0)
        return;
    v = on ? 1u : 0u;
    write(fd, &v, sizeof(v));
}

/* Drain up to `max` events; returns count (0 = empty). */
static int axgui_poll(int fd, struct axinput_event *ev, int max)
{
    long r;
    if (fd < 0 || !ev || max <= 0)
        return 0;
    r = read(fd, ev, (size_t)max * sizeof(*ev));
    if (r <= 0)
        return 0;
    return (int)((size_t)r / sizeof(*ev));
}

static void axgui_msleep(long ms)
{
    struct timespec ts;
    if (ms <= 0)
    {
        sys_yield();
        return;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

/* ---- 2D primitives on a 32-bit dumb buffer (canonical 0x00RRGGBB) ---- */
static void axgui_px(struct axgui_fb *fb, int x, int y, uint32_t rgb)
{
    if (!fb || !fb->px)
        return;
    if (x < 0 || y < 0 || (uint32_t)x >= fb->w || (uint32_t)y >= fb->h)
        return;
    fb->px[(size_t)y * (fb->pitch / 4) + (size_t)x] = rgb;
}

static void axgui_fill(struct axgui_fb *fb, int x, int y, int w, int h,
                       uint32_t rgb)
{
    int x0, y0, x1, y1, yy, xx;
    size_t stride;
    if (!fb || !fb->px || w <= 0 || h <= 0)
        return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > (int)fb->w ? (int)fb->w : x + w;
    y1 = y + h > (int)fb->h ? (int)fb->h : y + h;
    stride = fb->pitch / 4;
    for (yy = y0; yy < y1; yy++)
        for (xx = x0; xx < x1; xx++)
            fb->px[(size_t)yy * stride + (size_t)xx] = rgb;
}

static void axgui_rect(struct axgui_fb *fb, int x, int y, int w, int h,
                       uint32_t rgb)
{
    int i;
    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < w; i++)
    {
        axgui_px(fb, x + i, y, rgb);
        axgui_px(fb, x + i, y + h - 1, rgb);
    }
    for (i = 0; i < h; i++)
    {
        axgui_px(fb, x, y + i, rgb);
        axgui_px(fb, x + w - 1, y + i, rgb);
    }
}

static void axgui_glyph(struct axgui_fb *fb, char c, int x, int y,
                        uint32_t fg, uint32_t bg)
{
    unsigned int idx;
    int row, col;
    if ((unsigned char)c < FONT_FIRST_CHAR || (unsigned char)c > FONT_LAST_CHAR)
        c = '?';
    idx = (unsigned int)((unsigned char)c - FONT_FIRST_CHAR);
    if (idx >= FONT_NUM_CHARS)
        return;
    for (row = 0; row < FONT_HEIGHT; row++)
    {
        uint8_t bits = font8x16[idx][row];
        for (col = 0; col < FONT_WIDTH; col++)
            axgui_px(fb, x + col, y + row,
                     (bits & (1 << (7 - col))) ? fg : bg);
    }
}

static void axgui_text(struct axgui_fb *fb, const char *s, int x, int y,
                       uint32_t fg, uint32_t bg)
{
    int cx = x;
    if (!s)
        return;
    while (*s)
    {
        if (*s == '\n')
        {
            cx = x;
            y += FONT_HEIGHT;
        }
        else
        {
            axgui_glyph(fb, *s, cx, y, fg, bg);
            cx += FONT_WIDTH;
        }
        s++;
    }
}

/* Fixed-width text clipped to `max_chars` cells (pads with bg). */
static void axgui_text_cell(struct axgui_fb *fb, const char *s, int max_chars,
                            int x, int y, uint32_t fg, uint32_t bg)
{
    int i;
    for (i = 0; i < max_chars; i++)
    {
        char c = s[i];
        if (!c)
            c = ' ';
        axgui_glyph(fb, c, x + i * FONT_WIDTH, y, fg, bg);
    }
}

static int axgui_text_width(const char *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (*s++)
        n++;
    return n * FONT_WIDTH;
}

/* 12x12 mouse pointer (white arrow, black outline). */
static void axgui_cursor(struct axgui_fb *fb, int x, int y)
{
    static const char *shape[12] = {
        "X           ",
        "XX          ",
        "XXX         ",
        "XXXX        ",
        "XXXXX       ",
        "XXXXXX      ",
        "XXXXXXX     ",
        "XXXXXXXX    ",
        "XXXXXXXXX   ",
        "XXXXXX      ",
        "XX XXX      ",
        "   XXX      ",
    };
    int r, c;
    for (r = 0; r < 12; r++)
        for (c = 0; shape[r][c]; c++)
            if (shape[r][c] == 'X')
                axgui_px(fb, x + c, y + r, 0xFFFFFFu);
}

/* ---- run a command line, capturing stdout into `out` ----
   Same fd-dance as sh's pipelines: stdout is temporarily pointed at a pipe
   whose write end the spawned child inherits; EOF arrives when the child
   exits. Returns bytes captured (may be truncated at cap-1, NUL-terminated)
   or -1 if the program is unknown. One-shot commands only: an interactive
   child would block on the real TTY, which the GUI grabbed away. */
static long axgui_run_capture(const char *cmd, char *out, size_t cap)
{
    int fds[2];
    int sav;
    long pid;
    size_t got = 0;
    size_t cmdlen;
    if (!cmd || !out || cap == 0)
        return -1;
    out[0] = 0;
    cmdlen = strlen(cmd);
    if (cmdlen == 0)
        return 0;
    if (pipe(fds) < 0)
        return -1;
    sav = dup2(1, 9);
    if (sav < 0)
        sav = 1;
    dup2(fds[1], 1);
    close(fds[1]);
    pid = sys_spawn_cmd(cmd, cmdlen);
    dup2(sav, 1);
    if (sav != 1)
        close(sav);
    if (pid < 0)
    {
        close(fds[0]);
        return -1;
    }
    for (;;)
    {
        char tmp[256];
        long r = read(fds[0], tmp, sizeof(tmp));
        if (r <= 0)
            break;
        if (got + (size_t)r < cap - 1)
        {
            size_t i;
            for (i = 0; i < (size_t)r; i++)
                out[got++] = tmp[i];
        }
        else
        {
            /* Truncate but keep draining so the child never blocks on a
               full pipe; the in-kernel buffer grows anyway, this just
               hurries the common small-output case. */
            size_t room = (cap - 1 > got) ? (cap - 1 - got) : 0;
            size_t i;
            for (i = 0; i < room; i++)
                out[got++] = tmp[i];
        }
    }
    out[got] = 0;
    close(fds[0]);
    {
        int status = 0;
        sys_waitpid((int)pid, &status);
    }
    return (long)got;
}

/* Spawn + wait with fds untouched (for exclusive fullscreen GUI apps). */
static long axgui_run_wait(const char *cmd)
{
    long pid;
    int status = 0;
    if (!cmd || !*cmd)
        return -1;
    pid = sys_spawn_cmd(cmd, strlen(cmd));
    if (pid < 0)
        return -1;
    sys_waitpid((int)pid, &status);
    return (long)status;
}

/* ---- compositor <-> client transport (wm_abi.h) ---- */

/* Attach a window SHM segment and wire a draw-only framebuffer over its
   pixels (fd stays -1: the compositor owns PRESENT). Returns the header. */
static struct wm_win_hdr *axgui_win_attach(long shmid, struct axgui_fb *fb)
{
    uint8_t *base;
    struct wm_win_hdr *hdr;
    if (!fb || shmid <= 0)
        return 0;
    base = (uint8_t *)shm_attach(shmid);
    if (!base)
        return 0;
    hdr = (struct wm_win_hdr *)base;
    if (hdr->magic != WM_MAGIC || hdr->w == 0 || hdr->h == 0)
        return 0;
    fb->fd = -1;
    fb->w = hdr->w;
    fb->h = hdr->h;
    fb->pitch = hdr->w * 4;
    fb->handle = 0;
    fb->size = (uint64_t)hdr->w * hdr->h * 4;
    fb->px = (uint32_t *)(base + WM_HDR_SIZE);
    return hdr;
}

/* Full-write one event (pipes are byte streams; short writes must loop). */
static int axgui_wm_send(int fd, const struct wm_event *ev)
{
    size_t got = 0;
    const uint8_t *p;
    if (fd < 0 || !ev)
        return -1;
    p = (const uint8_t *)ev;
    while (got < sizeof(*ev))
    {
        long r = write(fd, p + got, sizeof(*ev) - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Blocking full-read of one event. Returns 0 on success, -1 on EOF/error
   (the compositor closed the window). */
static int axgui_wm_recv(int fd, struct wm_event *ev)
{
    size_t got = 0;
    uint8_t *p;
    if (fd < 0 || !ev)
        return -1;
    p = (uint8_t *)ev;
    while (got < sizeof(*ev))
    {
        long r = read(fd, p + got, sizeof(*ev) - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

#endif
