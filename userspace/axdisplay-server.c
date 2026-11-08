/* axdisplay-server.c — axiome display server (compositor).
   Owns /dev/dri/card0 (DRM master) and /devices/axw0 (protocol server).
   Composites client surfaces onto the scanout and page-flips.
   Single session, GOP backend only. Ports the guixd/axwm desktop shell
   (decorations, top bar, menu, input dispatch, drag/close) onto the axw
   protocol. */

#include "axw-protocol.h"
#include "axgui.h"
#include "syscall.h"
#include "drm/drm_ioctl.h"
#include "signal.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include <stddef.h>
#include <stdint.h>

/* ---- shell geometry (matches old guixd/axwm) ---- */
#define TOPBAR 24
#define TITLEBAR 20
#define FRAME_X 8
#define FRAME_TOP (TITLEBAR + 6)
#define FRAME_BOT 6

#define CURS_W 12
#define CURS_H 12

/* Palette (canonical 0x00RRGGBB, matches old compositors). */
#define D_BG     0x1B2A4Au
#define D_BG2    0x16233Cu
#define D_BAR    0x0F1722u
#define D_WinBG  0x101418u
#define D_Title  0x2B3B55u
#define D_TitleF 0x3D5A80u
#define D_FG     0xD8DEE9u
#define D_Prompt 0x88C0D0u
#define D_Accent 0x5E81ACu
#define D_Close  0xBF616Au
#define D_Menu   0x1E2836u
#define D_Dim    0x4C566Au

/* ---- DRM ioctl wrappers (raw, since we're the server) ---- */

static int drm_fd = -1;

static int drm_ioctl_raw(uint32_t op, void *arg, size_t argsz)
{
    struct drm_cmd cmd;
    cmd.magic = DRM_IOCTL_MAGIC;
    cmd.op = op;
    size_t cp = argsz < sizeof(cmd.args) ? argsz : sizeof(cmd.args);
    __builtin_memcpy(cmd.args, arg, cp);
    long r = write(drm_fd, &cmd, sizeof(cmd));
    if (r != (long)sizeof(cmd))
        return -1;
    __builtin_memcpy(arg, cmd.args, cp);
    return 0;
}

static int drm_get_mode(uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp)
{
    uint32_t out[5];
    long r = read(drm_fd, out, sizeof(out));
    if (r != (long)sizeof(out))
        return -1;
    if (w) *w = out[0];
    if (h) *h = out[1];
    if (pitch) *pitch = out[2];
    if (bpp) *bpp = out[3];
    return 0;
}

static int drm_gem_create(uint32_t size, uint32_t *handle)
{
    struct drm_gem_create gc;
    gc.size = size;
    gc.handle = 0;
    gc.pad = 0;
    if (drm_ioctl_raw(DRM_IOCTL_GEM_CREATE, &gc, sizeof(gc)) < 0)
        return -1;
    *handle = gc.handle;
    return 0;
}

static int drm_page_flip(uint32_t crtc_id, uint32_t fb_id, uint32_t flags)
{
    struct drm_mode_page_flip pf;
    pf.crtc_id = crtc_id;
    pf.fb_id = fb_id;
    pf.flags = flags;
    pf.pad = 0;
    return drm_ioctl_raw(DRM_IOCTL_PAGE_FLIP, &pf, sizeof(pf));
}

/* ---- server state ---- */

static int axw_fd = -1;          /* /devices/axw0 (server side, broadcasts to clients) */
static int g_input_fd = -1;      /* /devices/input0 for cursor polling */
static struct axgui_fb scanout;  /* scanout framebuffer */
static uint32_t scanout_handle = 0;
static uint32_t scanout_w = 0, scanout_h = 0;
static uint32_t crtc_id = 1;

#define MAX_CLIENTS 16
#define MAX_SURFACES 32
#define MAX_BUFFERS 32

struct client {
    int used;
    uint32_t conn_id;
};

struct surface {
    int used;
    uint32_t id;
    long pid;               /* client that owns this surface, -1 if none */
    int32_t x, y;               /* frame origin */
    uint32_t w, h;              /* content size */
    uint32_t buffer_id;
    int dirty;
    int32_t dirty_x, dirty_y, dirty_w, dirty_h;
    char title[48];
    int focused;
    int dragging;
    int drag_ox, drag_oy;
    int dead;
};

struct buffer {
    int used;
    uint32_t id;
    long shmid;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t *map;   /* shm_attach()'d pointer */
};

static struct client g_clients[MAX_CLIENTS];
static struct surface g_surfaces[MAX_SURFACES];
static struct buffer g_buffers[MAX_BUFFERS];
static uint32_t g_next_surface_id = 1;
static uint32_t g_next_buffer_id = 1;

/* ---- cursor state ---- */
static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;
static uint32_t g_cursor_btn = 0;
static uint8_t g_cursor_save[CURS_W * CURS_H];
static int g_cursor_save_ok = 0;
static int g_cursor_save_x = 0, g_cursor_save_y = 0;

/* ---- shell state ---- */
static int g_menu = 0;
static char g_apps[16][64];
static int g_napps = 0;
static int g_focus_idx = -1;    /* index into g_surfaces */
static int g_drag_idx = -1;     /* index into g_surfaces being dragged */

/* ---- render flag: set by protocol handlers, cleared after render ---- */
static int g_need_render = 0;

/* ---- spawn tracking: prevent auto-spawn storm while a process starts up ---- */
static int g_spawn_pending = 0;     /* 1 = a spawn is in flight, awaiting attach */
static long g_spawn_pending_since = 0;

/* ---- helper: send an axw event to all clients (server broadcasts) ---- */

static void axw_send_event(uint32_t sender_id, uint16_t opcode,
                           const uint32_t *args, int nargs)
{
    if (axw_fd < 0 || nargs < 0 || nargs > AXW_MAX_ARGS)
        return;
    uint8_t buf[8 + AXW_MAX_ARGS * 4];
    struct axw_msg_hdr *hdr = (struct axw_msg_hdr *)buf;
    hdr->sender_id = sender_id;
    hdr->opcode = opcode;
    hdr->size = (uint16_t)(8u + (uint32_t)nargs * 4u);
    if (nargs > 0)
        __builtin_memcpy(buf + 8, args, (size_t)nargs * 4u);
    write(axw_fd, buf, hdr->size);
}

static void send_output_events(void)
{
    /* Announce output geometry + mode to all bound clients. */
    uint32_t a[7] = { 0, 0, (int32_t)scanout_w, (int32_t)scanout_h, 0, 0, 0 };
    axw_send_event(AXW_ID_OUTPUT, AXW_OUTPUT_GEOMETRY, a, 7);
    uint32_t m[4] = { 1u, (int32_t)scanout_w, (int32_t)scanout_h, 60000u };
    axw_send_event(AXW_ID_OUTPUT, AXW_OUTPUT_MODE, m, 4);
    axw_send_event(AXW_ID_OUTPUT, AXW_OUTPUT_DONE, 0, 0);
}

/* ---- surface management ---- */

static struct surface *surface_find(uint32_t id)
{
    for (int i = 0; i < MAX_SURFACES; i++)
        if (g_surfaces[i].used && g_surfaces[i].id == id)
            return &g_surfaces[i];
    return 0;
}

static struct surface *surface_alloc(uint32_t id, long pid)
{
    for (int i = 0; i < MAX_SURFACES; i++)
    {
        if (!g_surfaces[i].used)
        {
            struct surface *s = &g_surfaces[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->id = id;
            s->pid = pid;
            s->x = 60 + (i % 4) * 28;
            s->y = TOPBAR + 30 + (i % 4) * 24;
            if (s->x + (int)(FRAME_X * 2) > (int)scanout.w - 8)
                s->x = (int)scanout.w - 8 - (int)(FRAME_X * 2);
            if (s->x < 0) s->x = 0;
            if (s->y + (int)(FRAME_TOP + FRAME_BOT) > (int)scanout.h - 8)
                s->y = (int)scanout.h - 8 - (int)(FRAME_TOP + FRAME_BOT);
            if (s->y < TOPBAR) s->y = TOPBAR;
            snprintf(s->title, sizeof(s->title), "Window");
            return s;
        }
    }
    return 0;
}

static void surface_free(struct surface *s)
{
    if (!s) return;
    s->used = 0;
    s->dead = 1;
    if (g_focus_idx >= 0 && &g_surfaces[g_focus_idx] == s)
        g_focus_idx = -1;
    if (g_drag_idx >= 0 && &g_surfaces[g_drag_idx] == s)
        g_drag_idx = -1;
}

static void surface_focus(int idx)
{
    if (idx < 0 || idx >= MAX_SURFACES || !g_surfaces[idx].used)
        return;
    for (int i = 0; i < MAX_SURFACES; i++)
        g_surfaces[i].focused = 0;
    g_surfaces[idx].focused = 1;
    g_focus_idx = idx;
}

static void send_pointer_event(struct surface *target, uint16_t opcode,
                               int32_t x, int32_t y, uint32_t btn)
{
    if (!target) return;
    uint32_t args[3] = { target->id,
                         (uint32_t)((uint32_t)x & 0xFFFFu) |
                         (uint32_t)(((uint32_t)y & 0xFFFFu) << 16),
                         btn };
    axw_send_event(target->id, opcode, args, 3);
}

static int surface_find_at(int x, int y)
{
    for (int i = 0; i < MAX_SURFACES; i++)
    {
        struct surface *s = &g_surfaces[i];
        if (!s->used || s->dead)
            continue;
        if (x >= s->x && x < s->x + (int)(s->w + FRAME_X * 2) &&
            y >= s->y && y < s->y + (int)(FRAME_TOP + s->h + FRAME_BOT))
            return i;
    }
    return -1;
}

/* ---- buffer management ---- */

static struct buffer *buffer_find(uint32_t id)
{
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (g_buffers[i].used && g_buffers[i].id == id)
            return &g_buffers[i];
    return 0;
}

static struct buffer *buffer_alloc(uint32_t id, long shmid,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch)
{
    (void)pitch;
    for (int i = 0; i < MAX_BUFFERS; i++)
    {
        if (!g_buffers[i].used)
        {
            struct buffer *b = &g_buffers[i];
            memset(b, 0, sizeof(*b));
            b->used = 1;
            b->id = id;
            b->shmid = shmid;
            b->width = width;
            b->height = height;
            b->pitch = width * 4;
            b->map = (uint8_t *)shm_attach(shmid);
            if (!b->map)
            {
                memset(b, 0, sizeof(*b));
                return 0;
            }
            return b;
        }
    }
    return 0;
}

static void buffer_free(struct buffer *b)
{
    if (!b) return;
    b->used = 0;
    b->map = 0;
}

/* ---- cursor snapshot/restore ---- */
static void cursor_snapshot(struct axgui_fb *fb, int x, int y)
{
    int r, c;
    if (!fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
    {
        for (c = 0; c < CURS_W; c++)
        {
            int px = x + c, py = y + r;
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
                g_cursor_save[(size_t)r * CURS_W + c] = 0;
            else
                g_cursor_save[(size_t)r * CURS_W + c] =
                    fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px];
        }
    }
    g_cursor_save_x = x;
    g_cursor_save_y = y;
    g_cursor_save_ok = 1;
}

static void cursor_restore(struct axgui_fb *fb)
{
    int r, c;
    if (!g_cursor_save_ok || !fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
    {
        for (c = 0; c < CURS_W; c++)
        {
            int px = g_cursor_save_x + c, py = g_cursor_save_y + r;
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
                continue;
            fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px] =
                g_cursor_save[(size_t)r * CURS_W + c];
        }
    }
    g_cursor_save_ok = 0;
}

static void draw_cursor(struct axgui_fb *fb, int x, int y)
{
    axgui_cursor(fb, x, y);
}

/* ---- protocol message reading ---- */

static int axw_read_message(int fd, struct axw_msg_hdr *hdr, uint32_t *args)
{
    uint8_t buf[8 + AXW_MAX_ARGS * 4];
    long r = read(fd, buf, sizeof(buf));
    if (r <= 0)
        return -1;
    if (r < (long)sizeof(*hdr))
        return -1;
    __builtin_memcpy(hdr, buf, sizeof(*hdr));
    uint16_t total = hdr->size;
    if (r < (long)total)
        return -1;
    int nargs = (int)((total - 8u) / 4u);
    if (nargs > 0)
        __builtin_memcpy(args, buf + 8, (size_t)nargs * 4u);
    return nargs;
}

/* ---- protocol message handling ---- */

static void handle_get_registry(uint32_t sender_id, const uint32_t *args, int nargs)
{
    (void)sender_id; (void)args; (void)nargs;
    /* Globals are well-known and always available. */
}

static void handle_create_surface(uint32_t sender_id, const uint32_t *args, int nargs)
{
    (void)sender_id;
    if (nargs < 1)
        return;
    uint32_t surface_id = args[0];
    if (surface_find(surface_id))
        return;
    /* Surface is fully created when the client attaches a buffer.
       This avoids orphan surfaces when a client dies before attaching. */
    g_need_render = 1;
}

static void handle_create_pool(uint32_t sender_id, const uint32_t *args, int nargs)
{
    (void)sender_id;
    if (nargs < 5)
        return;
    uint32_t buffer_id = args[0];
    long shmid = (long)args[1];
    uint32_t width = args[2];
    uint32_t height = args[3];
    uint32_t format = args[4];
    (void)format;
    if (buffer_find(buffer_id))
        return;
    struct buffer *b = buffer_alloc(buffer_id, shmid, width, height, 0);
    if (!b)
        return;
}

static void handle_attach(uint32_t sender_id, const uint32_t *args, int nargs)
{
    if (nargs < 3)
        return;
    uint32_t surface_id = args[0];
    uint32_t buffer_id = args[1];
    uint32_t pos = args[2];
    int32_t x = (int32_t)(pos & 0xFFFFu);
    int32_t y = (int32_t)((pos >> 16) & 0xFFFFu);

    struct surface *s = surface_find(surface_id);
    if (!s)
    {
        /* Create the surface now that a buffer is being attached. */
        s = surface_alloc(surface_id, -1);
        if (!s)
            return;
    }
    struct buffer *b = buffer_find(buffer_id);
    if (!b)
        return;

    s->buffer_id = buffer_id;
    s->x = x;
    s->y = y;
    /* Keep windows below the topbar so the title bar stays accessible. */
    if (s->y < TOPBAR) s->y = TOPBAR;
    if (s->x < 0) s->x = 0;
    if ((int)(s->x + s->w + FRAME_X * 2) > (int)scanout.w - 8)
        s->x = (int)scanout.w - 8 - (int)(s->w + FRAME_X * 2);
    if (s->x < 0) s->x = 0;
    if ((int)(s->y + FRAME_TOP + s->h + FRAME_BOT) > (int)scanout.h - 8)
        s->y = (int)scanout.h - 8 - (int)(FRAME_TOP + s->h + FRAME_BOT);
    if (s->y < TOPBAR) s->y = TOPBAR;
    s->w = b->width;
    s->h = b->height;
    s->dirty = 1;
    s->dirty_x = 0;
    s->dirty_y = 0;
    s->dirty_w = b->width;
    s->dirty_h = b->height;
    /* A surface just attached: the spawned process is now running. */
    g_spawn_pending = 0;
    g_need_render = 1;
}

static void handle_damage(uint32_t sender_id, const uint32_t *args, int nargs)
{
    (void)sender_id;
    if (nargs < 3)
        return;
    uint32_t surface_id = args[0];
    uint32_t xy = args[1];
    uint32_t wh = args[2];
    int32_t x = (int32_t)(xy & 0xFFFFu);
    int32_t y = (int32_t)((xy >> 16) & 0xFFFFu);
    int32_t w = (int32_t)(wh & 0xFFFFu);
    int32_t h = (int32_t)((wh >> 16) & 0xFFFFu);

    struct surface *s = surface_find(surface_id);
    if (!s)
        return;
    s->dirty = 1;
    s->dirty_x = x;
    s->dirty_y = y;
    s->dirty_w = w;
    s->dirty_h = h;
    g_need_render = 1;
}

static void handle_commit(uint32_t sender_id, const uint32_t *args, int nargs)
{
    (void)nargs;
    if (nargs < 1)
        return;
    uint32_t surface_id = args[0];
    struct surface *s = surface_find(surface_id);
    if (!s)
        return;
    s->dirty = 1;
    g_need_render = 1;
}

static void handle_surface_destroy(uint32_t sender_id)
{
    struct surface *s = surface_find(sender_id);
    if (s)
        surface_free(s);
    g_need_render = 1;
}

/* ---- rendering ---- */

static void blit_buffer_to_fb(struct axgui_fb *dst, struct buffer *b,
                              int32_t dx, int32_t dy,
                              int32_t sx, int32_t sy,
                              int32_t cw, int32_t ch)
{
    if (!b || !b->map || cw <= 0 || ch <= 0)
        return;
    for (int32_t row = 0; row < ch; row++)
    {
        uint32_t *srow = (uint32_t *)(b->map + (size_t)(sy + row) * b->pitch);
        uint32_t *drow = &dst->px[(size_t)(dy + row) * (dst->pitch / 4) + (size_t)dx];
        for (int32_t col = 0; col < cw; col++)
            drow[col] = srow[col];
    }
}

static void composite_surface(struct axgui_fb *dst, struct surface *s)
{
    if (!s->used)
        return;

    int cx = s->x;
    int cy = s->y;
    int frame_w = (int)(s->w + FRAME_X * 2);
    int frame_h = (int)(FRAME_TOP + s->h + FRAME_BOT);

    /* Clamp to scanout. */
    if (cx < 0) { frame_w += cx; cx = 0; }
    if (cy < 0) { frame_h += cy; cy = 0; }
    if (cx + frame_w > (int)dst->w) frame_w = dst->w - cx;
    if (cy + frame_h > (int)dst->h) frame_h = dst->h - cy;
    if (frame_w <= 0 || frame_h <= 0) return;

    uint32_t tc = s->focused ? D_TitleF : D_Title;

    /* Window background + shadow area. */
    axgui_fill(dst, cx, cy, frame_w, frame_h, D_WinBG);

    /* Title bar. */
    axgui_fill(dst, cx, cy, frame_w, TITLEBAR, tc);

    /* Border. */
    axgui_rect(dst, cx, cy, frame_w, frame_h, D_Accent);

    /* Title text. */
    axgui_text(dst, s->title, cx + 8, cy + 2, D_FG, tc);

    /* Close button. */
    axgui_fill(dst, cx + (int)s->w - TITLEBAR, cy + 3, 14, 14, D_Close);
    axgui_text(dst, "x", cx + (int)s->w - TITLEBAR + 3, cy + 1, D_FG, D_Close);

    /* Content area. */
    int content_x = cx + FRAME_X;
    int content_y = cy + FRAME_TOP;
    int content_w = (int)s->w;
    int content_h = (int)s->h;

    struct buffer *b = buffer_find(s->buffer_id);
    if (b && b->map)
    {
        int cw = content_w, ch = content_h;
        int sx = 0, sy = 0;
        if (content_x < 0) { cw += content_x; sx = -content_x; content_x = 0; }
        if (content_y < 0) { ch += content_y; sy = -content_y; content_y = 0; }
        if (content_x + cw > (int)dst->w) cw = dst->w - content_x;
        if (content_y + ch > (int)dst->h) ch = dst->h - content_y;
        if (cw > 0 && ch > 0)
            blit_buffer_to_fb(dst, b, content_x, content_y, sx, sy, cw, ch);
        s->dirty = 0;
    }
    else if (!b || !b->map)
    {
        axgui_fill(dst, content_x, content_y, content_w, content_h, D_WinBG);
    }
}

static void render_topbar(struct axgui_fb *fb)
{
    long now = (long)time(0);
    char clock[32];
    int hh, mm, ss;
    axgui_fill(fb, 0, 0, (int)fb->w, TOPBAR, D_BAR);
    axgui_fill(fb, 4, 3, 92, 18, g_menu ? D_Accent : D_Title);
    axgui_text(fb, "axiome", 12, 4, D_FG, g_menu ? D_Accent : D_Title);
    if (now < 0) now = 0;
    ss = (int)((now % 60 + 60) % 60);
    mm = (int)(((now / 60) % 60 + 60) % 60);
    hh = (int)(((now / 3600) % 24 + 24) % 24);
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d UTC", hh, mm, ss);
    axgui_text(fb, clock,
               (int)fb->w / 2 - axgui_text_width(clock) / 2, 4, D_FG, D_BAR);
    axgui_fill(fb, (int)fb->w - 120, 3, 56, 18, D_Title);
    axgui_text(fb, "+Term", (int)fb->w - 112, 4, D_FG, D_Title);
}

static void render_menu(struct axgui_fb *fb)
{
    int mh = (5 + g_napps + 1) * FONT_HEIGHT + 8;
    int my = TOPBAR + 2;
    int row = 0;
    axgui_fill(fb, 4, my, 240, mh, D_Menu);
    axgui_rect(fb, 4, my, 240, mh, D_Accent);
    axgui_text(fb, "+ New terminal", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Prompt, D_Menu);
    axgui_text(fb, "= System info", 12, my + 4 + row++ * FONT_HEIGHT,
               D_FG, D_Menu);
    axgui_text(fb, "R Restart", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Prompt, D_Menu);
    axgui_text(fb, "P Power off", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Close, D_Menu);
    for (int i = 0; i < g_napps; i++)
    {
        char entry[80];
        snprintf(entry, sizeof(entry), "  %s", g_apps[i]);
        axgui_text(fb, entry, 12, my + 4 + row++ * FONT_HEIGHT, D_FG, D_Menu);
    }
}

static void render_frame(void)
{
    cursor_restore(&scanout);

    /* Clear scanout. */
    axgui_fill(&scanout, 0, 0, (int)scanout.w, (int)scanout.h, D_BG);
    for (int y = TOPBAR + 16; y < (int)scanout.h; y += 32)
        for (int x = 16; x < (int)scanout.w; x += 32)
            axgui_px(&scanout, x, y, D_BG2);

    /* Composite all surfaces (back to front by index). */
    for (int i = 0; i < MAX_SURFACES; i++)
        composite_surface(&scanout, &g_surfaces[i]);

    /* Top bar + menu (only when no fullscreen surface). */
    int has_fullscreen = 0;
    (void)has_fullscreen;
    render_topbar(&scanout);
    if (g_menu)
        render_menu(&scanout);

    /* Draw cursor. */
    cursor_snapshot(&scanout, g_cursor_x, g_cursor_y);
    draw_cursor(&scanout, g_cursor_x, g_cursor_y);

    /* Present via DRM page flip. */
    drm_page_flip(crtc_id, scanout_handle, 0);
}

/* ---- app scanning ---- */

static void scan_apps(void)
{
    struct vfs_dirent ents[40];
    int n, i;
    g_napps = 0;
    n = readdir("/Binaries", ents, 40);
    if (n <= 0)
        return;
    for (i = 0; i < n && g_napps < 16; i++)
    {
        const char *nm = ents[i].name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (strcmp(nm, "init") == 0 || strcmp(nm, "syslogd") == 0 ||
            strcmp(nm, "oobe") == 0 || strcmp(nm, "login") == 0 ||
            strcmp(nm, "axwm") == 0 || strcmp(nm, "guixd") == 0 ||
            strcmp(nm, "axlogin") == 0 || strcmp(nm, "axoobe") == 0 ||
            strcmp(nm, "evtest") == 0)
            continue;
        strncpy(g_apps[g_napps], nm, 63);
        g_apps[g_napps][63] = 0;
        g_napps++;
    }
}

/* ---- client spawning ---- */

static long g_last_spawn = 0;
static int g_spawn_fails = 0;

/* Explicit user action - always allowed. */
static int spawn_client_now(const char *title, const char *prog,
                            const char *initial, long uid, long gid)
{
    (void)uid; (void)gid;
    char cmd[256];
    int r;
    if (initial && initial[0])
        r = snprintf(cmd, sizeof(cmd), "%s%s%s", prog,
                     initial[0] ? " " : "", initial);
    else
        r = snprintf(cmd, sizeof(cmd), "%s", prog);
    if (r < 0 || (size_t)r >= sizeof(cmd))
        return 0;
    long pid = sys_spawn_cmd(cmd, (size_t)r);
    if (pid < 0)
    {
        g_spawn_fails++;
        return 0;
    }
    g_spawn_fails = 0;
    g_last_spawn = (long)time(0);
    g_spawn_pending = 1;
    g_spawn_pending_since = g_last_spawn;
    (void)title;
    return 1;
}

/* Auto-spawn (empty desktop) - rate limited to avoid spam storms. */
static int spawn_client_auto(const char *title, const char *prog,
                             const char *initial, long uid, long gid)
{
    (void)uid; (void)gid;
    long now = (long)time(0);
    /* Don't auto-spawn while a previous spawn is still starting up. */
    if (g_spawn_pending)
    {
        /* If pending for too long (>10s), give up and allow re-spawn. */
        if (now - g_spawn_pending_since > 10)
        {
            g_spawn_pending = 0;
        }
        else
        {
            return 0;
        }
    }
    if (now - g_last_spawn < 1)
        return 0;
    return spawn_client_now(title, prog, initial, uid, gid);
}

/* ---- click handling ---- */

static int handle_click(int x, int y, long now)
{
    (void)now;
    if (y >= 0 && y < TOPBAR)
    {
        if (x >= 4 && x < 96)
        {
            g_menu = !g_menu;
            return 1;
        }
        if (x >= (int)scanout.w - 120 && x < (int)scanout.w - 64)
        {
            spawn_client_now("Terminal", "axterm", 0, -1, -1);
            g_menu = 0;
            return 1;
        }
        return 1;
    }
    if (g_menu && x >= 4 && x < 244)
    {
        int idx = (y - TOPBAR - 4) / FONT_HEIGHT;
        int row = 0;
        if (idx == row++)
        {
            spawn_client_now("Terminal", "axterm", 0, -1, -1);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            spawn_client_now("System info", "axinfo", 0, -1, -1);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            g_menu = 0;
            sys_reboot();
            return 1;
        }
        if (idx == row++)
        {
            g_menu = 0;
            sys_poweroff();
            return 1;
        }
        for (int i = 0; i < g_napps; i++)
        {
            if (idx == row++)
            {
                spawn_client_now(g_apps[i], "axterm", "-c", -1, -1);
                g_menu = 0;
                return 1;
            }
        }
        return 1;
    }
    g_menu = 0;
    int si = surface_find_at(x, y);
    if (si < 0)
        return 0;
    struct surface *s = &g_surfaces[si];
    if (!s->used)
        return 0;

    surface_focus(si);

    int xbtn = (x >= s->x + (int)s->w - TITLEBAR &&
                x < s->x + (int)s->w - 4 &&
                y >= s->y + 3 && y < s->y + TITLEBAR - 3);
    if (xbtn)
    {
        uint32_t args[1] = { s->id };
        axw_send_event(s->id, AXW_SURFACE_DESTROY, args, 1);
        surface_free(s);
        g_need_render = 1;
        return 1;
    }

    if (y >= s->y && y < s->y + TITLEBAR)
    {
        s->dragging = 1;
        s->drag_ox = x - s->x;
        s->drag_oy = y - s->y;
        g_drag_idx = si;
    }
    return 1;
}

/* ---- main loop ---- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("axdisplay: starting display server\n");

    /* Open DRM device. */
    drm_fd = open("/dev/dri/card0", 2);
    if (drm_fd < 0)
        drm_fd = open("/devices/dri0", 2);
    if (drm_fd < 0)
    {
        printf("axdisplay: cannot open DRM device\n");
        return 1;
    }

    /* Become DRM master. */
    {
        struct drm_cmd cmd;
        cmd.magic = DRM_IOCTL_MAGIC;
        cmd.op = DRM_IOCTL_SET_MASTER;
        cmd.args[0] = 0; cmd.args[1] = 0; cmd.args[2] = 0;
        cmd.args[3] = 0; cmd.args[4] = 0; cmd.args[5] = 0;
        write(drm_fd, &cmd, sizeof(cmd));
    }

    /* Get scanout mode. */
    if (drm_get_mode(&scanout_w, &scanout_h, 0, 0) < 0)
    {
        scanout_w = 640;
        scanout_h = 480;
    }
    printf("axdisplay: scanout %ux%u\n", scanout_w, scanout_h);

    /* Create scanout GEM buffer. */
    uint32_t gem_size = scanout_w * scanout_h * 4;
    if (drm_gem_create(gem_size, &scanout_handle) < 0)
    {
        printf("axdisplay: GEM create failed\n");
        return 1;
    }

    /* Map scanout buffer. */
    {
        void *map_hint = (void *)0x200000000000ULL;
        long mr = syscall(SYS_MMAP, (long)drm_fd,
                          (long)((uint64_t)scanout_handle << 32),
                          (long)map_hint, (long)gem_size, 0, 0);
        if (mr != 0)
        {
            printf("axdisplay: mmap failed\n");
            return 1;
        }
        scanout.fd = drm_fd;
        scanout.w = scanout_w;
        scanout.h = scanout_h;
        scanout.pitch = scanout_w * 4;
        scanout.handle = scanout_handle;
        scanout.size = gem_size;
        scanout.px = (uint32_t *)map_hint;
    }

    /* Open axw protocol device (becomes server since first opener). */
    axw_fd = open("/devices/axw0", 2);
    if (axw_fd < 0)
        axw_fd = open("/Devices/axw0", 2);
    if (axw_fd < 0)
    {
        printf("axdisplay: cannot open /devices/axw0\n");
        return 1;
    }
    fcntl(axw_fd, 4 /* F_SETFL */, 04000 /* O_NONBLOCK */);
    printf("axdisplay: protocol server up on /devices/axw0\n");

    /* Grab input. */
    g_input_fd = axgui_input_open();
    if (g_input_fd >= 0)
    {
        axgui_grab(g_input_fd, 1);
        axgui_poll(g_input_fd, 0, 0);
    }

    /* Announce output to clients. */
    send_output_events();

    /* Scan apps and seed desktop. */
    scan_apps();
    spawn_client_now("Terminal", "axterm", 0, -1, -1);

    uint64_t avg_us = AXGUI_TARGET_60_US;
    uint64_t frame_id = 0;

    for (;;)
    {
        uint64_t frame_start_us = axgui_now_us();
        long now = (long)time(0);
        int state_changed = 0;
        int mouse_moved = 0;

        /* Drain all pending axw protocol messages. */
        for (;;)
        {
            struct axw_msg_hdr hdr;
            uint32_t args[AXW_MAX_ARGS];
            int nargs = axw_read_message(axw_fd, &hdr, args);
            if (nargs < 0)
                break;
            state_changed = 1;

            if (hdr.sender_id == AXW_ID_DISPLAY)
            {
                if (hdr.opcode == AXW_DISPLAY_GET_REGISTRY)
                    handle_get_registry(hdr.sender_id, args, nargs);
                else if (hdr.opcode == AXW_DISPLAY_TERMINATE)
                {
                    /* Client disconnect: nothing to clean (no per-client state). */
                }
            }
            else if (hdr.sender_id == AXW_ID_COMPOSITOR)
            {
                if (hdr.opcode == AXW_COMPOSITOR_CREATE_SURFACE)
                    handle_create_surface(hdr.sender_id, args, nargs);
            }
            else if (hdr.sender_id == AXW_ID_SHM)
            {
                if (hdr.opcode == AXW_SHM_CREATE_POOL)
                    handle_create_pool(hdr.sender_id, args, nargs);
            }
            else if (hdr.sender_id & 0x80000000u)
            {
                /* Other server globals - ignore. */
            }
            else
            {
                switch (hdr.opcode)
                {
                    case AXW_SURFACE_ATTACH:
                        handle_attach(hdr.sender_id, args, nargs);
                        break;
                    case AXW_SURFACE_DAMAGE:
                        handle_damage(hdr.sender_id, args, nargs);
                        break;
                    case AXW_SURFACE_COMMIT:
                        handle_commit(hdr.sender_id, args, nargs);
                        break;
                    case AXW_SURFACE_DESTROY:
                        handle_surface_destroy(hdr.sender_id);
                        break;
                    default:
                        break;
                }
            }
        }

        /* Poll input. */
        {
            struct axinput_event ev[64];
            int n = axgui_poll(g_input_fd, ev, 64);
            for (int i = 0; i < n; i++)
            {
                if (ev[i].type == AXINPUT_TYPE_MOUSE)
                {
                    int nx = g_cursor_x + ev[i].dx;
                    int ny = g_cursor_y + ev[i].dy;
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if ((uint32_t)nx >= scanout.w) nx = (int)scanout.w - 1;
                    if ((uint32_t)ny >= scanout.h) ny = (int)scanout.h - 1;
                    if (nx != g_cursor_x || ny != g_cursor_y)
                        mouse_moved = 1;
                    g_cursor_x = nx;
                    g_cursor_y = ny;

                    uint32_t new_btn = ev[i].code;
                    uint32_t pressed = new_btn & ~g_cursor_btn;
                    uint32_t released = ~new_btn & g_cursor_btn;
                    g_cursor_btn = new_btn;

                    /* Hit-test surface under cursor for input dispatch. */
                    int hit_si = surface_find_at(g_cursor_x, g_cursor_y);
                    struct surface *hit = (hit_si >= 0) ? &g_surfaces[hit_si] : 0;

                    /* Surface-local coords (subtract frame offset). */
                    int32_t lx = 0, ly = 0;
                    if (hit)
                    {
                        lx = g_cursor_x - (hit->x + FRAME_X);
                        ly = g_cursor_y - (hit->y + FRAME_TOP);
                        surface_focus(hit_si);
                    }

                    if (pressed || released || mouse_moved)
                    {
                        if (hit)
                        {
                            if (pressed || released)
                            {
                                send_pointer_event(hit, AXW_POINTER_BUTTON,
                                                   lx, ly, new_btn);
                            }
                            else if (mouse_moved)
                            {
                                send_pointer_event(hit, AXW_POINTER_MOTION,
                                                   lx, ly, new_btn);
                            }
                        }
                    }

                    if (ev[i].code & AXINPUT_BTN_LEFT)
                    {
                        state_changed = 1;
                        if (g_drag_idx >= 0 && g_drag_idx < MAX_SURFACES)
                        {
                            struct surface *w = &g_surfaces[g_drag_idx];
                            if (w->used && w->dragging)
                            {
                                w->x = g_cursor_x - w->drag_ox;
                                w->y = g_cursor_y - w->drag_oy;
                                if (w->y < TOPBAR) w->y = TOPBAR;
                                if (w->x < -((int)(w->w + FRAME_X * 2) - 60))
                                    w->x = -((int)(w->w + FRAME_X * 2) - 60);
                                if (w->x > (int)scanout.w - 40)
                                    w->x = (int)scanout.w - 40;
                                if (w->y > (int)scanout.h - 40)
                                    w->y = (int)scanout.h - 40;
                                g_need_render = 1;
                            }
                        }
                    }
                    else if (g_drag_idx >= 0 && g_drag_idx < MAX_SURFACES)
                    {
                        struct surface *w = &g_surfaces[g_drag_idx];
                        if (w->used && w->dragging)
                        {
                            w->dragging = 0;
                            g_drag_idx = -1;
                        }
                    }
                }
                else if (ev[i].type == AXINPUT_TYPE_KEY)
                {
                    uint32_t kc = ev[i].code;
                    if (kc == '`')
                    {
                        g_menu = !g_menu;
                        state_changed = 1;
                    }
                    else if (g_focus_idx >= 0 && g_focus_idx < MAX_SURFACES)
                    {
                        struct surface *f = &g_surfaces[g_focus_idx];
                        if (f->used && !f->dead)
                        {
                            uint32_t args[3] = { f->id, kc, 0 };
                            axw_send_event(AXW_ID_SEAT, AXW_KEYBOARD_KEY, args, 2);
                        }
                    }
                }
            }
        }

        /* Handle click edge on button press. */
        {
            static uint32_t prev_btn = 0;
            if ((g_cursor_btn & AXINPUT_BTN_LEFT) && !(prev_btn & AXINPUT_BTN_LEFT))
            {
                if (handle_click(g_cursor_x, g_cursor_y, now))
                    state_changed = 1;
            }
            prev_btn = g_cursor_btn;
        }

        /* Reap: check if any client process exited.
           We track surfaces by creation order; a dead surface stays
           until the user clicks its close button. */

        /* Clock tick. */
        {
            static long last_clock = -1;
            if (now != last_clock)
            {
                last_clock = now;
                state_changed = 1;
            }
        }

        /* Render / present / cursor-only. */
        if (state_changed || mouse_moved || g_need_render)
        {
            render_frame();
            g_need_render = 0;
        }

        /* Adaptive pacing. */
        {
            uint64_t now_us = axgui_now_us();
            uint64_t elapsed_us = 0;
            uint64_t target_us;
            long remain_us;
            int active = state_changed || mouse_moved;
            if (now_us > frame_start_us)
                elapsed_us = now_us - frame_start_us;
            avg_us = (avg_us * 7 + elapsed_us) / 8;
            if (avg_us < 1000)
                avg_us = elapsed_us;
            target_us = axgui_auto_target_us(0, active, avg_us);
            frame_id++;
            if (elapsed_us >= target_us)
                sys_yield();
            else
            {
                remain_us = (long)(target_us - elapsed_us);
                if (!active && remain_us > 50000)
                    remain_us = 50000;
                axgui_sleep_us(remain_us);
            }
        }
    }

    for (;;)
        sys_yield();
    return 0;
}
