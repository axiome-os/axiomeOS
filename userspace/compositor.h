#ifndef AXIOME_COMPOSITOR_H
#define AXIOME_COMPOSITOR_H

/* Shared compositor core for axiomeOS.
   Extracted from guixd.c and axwm.c: window lifecycle, SHM pool, cursor
   snapshot/restore, seqlock blit, top bar, menu, input dispatch, resize.
   Both compositors include this header and call comp_* helpers from their
   own main(); compositor specific behaviour (sessions, login chaining,
   Exit to console) stays in the compositor's main().

   Header-only (like axgui.h / axclient.h): every function is static inline
   so each compositor gets its own copy without needing shared object
   linking in the Makefile. */

#include "axgui.h"
#include "wm_abi.h"
#include "signal.h"
#include <time.h>

/* Resize edge/corner identifiers. */
#define COMP_RESIZE_NONE   0
#define COMP_RESIZE_LEFT   1
#define COMP_RESIZE_RIGHT  2
#define COMP_RESIZE_TOP    3
#define COMP_RESIZE_BOTTOM 4
#define COMP_RESIZE_TL     5
#define COMP_RESIZE_TR     6
#define COMP_RESIZE_BL     7
#define COMP_RESIZE_BR     8

/* Resize border thickness (px). */
#define COMP_RESIZE_BORDER 6

struct comp_win {
    int used;
    int live;               /* ready seen: composited and eligible for input */
    int x, y;               /* frame origin */
    uint32_t w, h;          /* window content size (dynamic, <= max) */
    char title[48];
    int focused;
    int dragging;
    int resizing;
    int resize_edge;        /* COMP_RESIZE_* */
    int drag_ox, drag_oy;   /* drag: mouse offset from window origin */
    int resize_ox, resize_oy;   /* resize: mouse pos at resize start */
    int resize_win_x, resize_win_y; /* resize: window pos at resize start */
    uint32_t resize_ow, resize_oh;
    long shmid;
    struct wm_win_hdr *hdr; /* compositor mapping of the window SHM */
    long pid;               /* client pid, -1 when none/reaped */
    int evfd;               /* event pipe write end, -1 when closed */
    int dead;               /* client reaped; frame kept until dismissed */
    int closing;            /* close requested */
    int fullscreen;         /* fill entire screen, no title bar/border */
    int noclose;            /* cannot be closed via X button */
    int last_mx, last_my;   /* last mouse state forwarded */
    uint32_t last_btn;
    uint32_t seq;           /* last seqlock seq fully blitted */
    long ready_deadline;    /* time(0) by which ready must appear */
    long close_deadline;    /* time(0) when SIGKILL escalates a close */
    int chain_login;        /* when this client exits 0, open login */
    int is_login;           /* axlogin: a status-0 reap spawns its session */
    uint32_t gen;           /* slot generation handed to the client */
    int status;             /* exit status */
};

struct comp_ctx {
    struct axgui_fb fb;             /* scanout framebuffer */
    int input_fd;                   /* /Devices/input0 */
    struct axgui_caps caps;         /* gfx capabilities */
    int caps_ok;
    struct comp_win wins[WM_MAX_WIN];
    int z[WM_MAX_WIN];
    int nz;
    int menu;
    int mx, my;
    uint32_t btn;
    int drag_win;                   /* index into wins[] being dragged/resized */
    char apps[16][64];
    int napps;
    uint32_t gencnt;                /* slot generation counter */
    int queued_login;               /* open login after reap scan */
    long gfx_pid;                   /* async fullscreen demo pid */
    long empty_since;               /* time(0) when desktop last went empty */
    int spawn_fails;                /* consecutive auto-reopen failures */
    long sess_uid, sess_gid;        /* logged-in user */
};

/* ---- window lookup / focus ---- */

static int comp_win_at(struct comp_ctx *c, int x, int y)
{
    int i;
    for (i = c->nz - 1; i >= 0; i--)
    {
        struct comp_win *w = &c->wins[c->z[i]];
        if (!w->used)
            continue;
        if (w->fullscreen)
            return c->z[i];
        if (x >= w->x && x < w->x + (int)(w->w + FRAME_X * 2) &&
            y >= w->y && y < w->y + (int)(FRAME_TOP + w->h + FRAME_BOT))
            return c->z[i];
    }
    return -1;
}

static struct comp_win *comp_focused_win(struct comp_ctx *c)
{
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
        if (c->wins[i].used && c->wins[i].focused && !c->wins[i].dead)
            return &c->wins[i];
    return 0;
}

static void comp_win_focus(struct comp_ctx *c, struct comp_win *w)
{
    int i, k;
    if (!w || !w->used || w->dead)
        return;
    for (i = 0; i < WM_MAX_WIN; i++)
        c->wins[i].focused = (&c->wins[i] == w);
    k = 0;
    for (i = 0; i < c->nz; i++)
    {
        if (&c->wins[c->z[i]] != w)
            c->z[k++] = c->z[i];
    }
    c->z[k++] = (int)(w - c->wins);
    c->nz = k;
}

static void comp_elect_focus(struct comp_ctx *c)
{
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
        c->wins[i].focused = 0;
    for (i = c->nz - 1; i >= 0; i--)
    {
        struct comp_win *t = &c->wins[c->z[i]];
        if (t->used && !t->dead)
        {
            t->focused = 1;
            break;
        }
    }
}

/* ---- SHM pool ---- */

static int comp_shm_pool_create(struct comp_ctx *c)
{
    size_t win_bytes = WM_HDR_SIZE + (size_t)WM_SHM_MAX_W * WM_SHM_MAX_H * 4;
    int i;
    memset(c->wins, 0, sizeof(c->wins));
    for (i = 0; i < WM_MAX_WIN; i++)
    {
        long id;
        uint8_t *base;
        size_t k;
        c->wins[i].pid = -1;
        c->wins[i].evfd = -1;
        id = shm_create(win_bytes);
        if (id <= 0)
        {
            printf("compositor: shm_create failed for slot %d\n", i);
            continue;
        }
        base = (uint8_t *)shm_attach(id);
        if (!base)
        {
            printf("compositor: shm_attach failed for slot %d\n", i);
            continue;
        }
        for (k = 0; k < win_bytes; k++)
            base[k] = 0;
        c->wins[i].shmid = id;
        c->wins[i].hdr = (struct wm_win_hdr *)base;
        c->wins[i].hdr->pitch = (uint32_t)(WM_SHM_MAX_W * 4);
    }
    c->mx = (int)c->fb.w / 2;
    c->my = (int)c->fb.h / 2;
    c->gencnt = 1;
    return 0;
}

static void comp_shm_pool_destroy(struct comp_ctx *c)
{
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
    {
        c->wins[i].shmid = 0;
        c->wins[i].hdr = 0;
    }
}

/* Reallocate a window's SHM segment to a new size. Copies old content into
   the new buffer, frees the old segment, and updates the window's shmid /
   hdr. Returns 0 on success, -1 on failure. */
static int comp_shm_realloc(struct comp_ctx *c, struct comp_win *w,
                            uint32_t new_w, uint32_t new_h)
{
    size_t new_bytes = WM_HDR_SIZE + (size_t)new_w * new_h * 4;
    long new_id;
    uint8_t *new_base;
    struct wm_win_hdr *new_hdr;
    uint32_t old_w, old_h;
    uint32_t copy_w, copy_h;
    uint32_t r, cc;
    uint32_t s0, s1;
    int torn;

    if (!w || !w->hdr)
        return -1;
    old_w = w->hdr->w;
    old_h = w->hdr->h;

    new_id = shm_create(new_bytes);
    if (new_id <= 0)
        return -1;
    new_base = (uint8_t *)shm_attach(new_id);
    if (!new_base)
    {
        shm_destroy(new_id);
        return -1;
    }
    new_hdr = (struct wm_win_hdr *)new_base;
    new_hdr->magic = WM_MAGIC;
    new_hdr->w = new_w;
    new_hdr->h = new_h;
    new_hdr->closed = 0;
    new_hdr->seq = 0;
    new_hdr->ready = 0; /* not ready until copy is validated */
    new_hdr->gen = w->hdr->gen;
    new_hdr->gfx_caps = w->hdr->gfx_caps;
    new_hdr->gfx_detail = w->hdr->gfx_detail;
    new_hdr->pad = 0;
    new_hdr->pad2 = 0;
    new_hdr->pitch = new_w * 4;

    /* Seqlock-validated copy of old content. If the client is mid-draw
       (seq odd) or the copy tears, skip the copy: new buffer is zeroed
       and the client will repaint on its next frame. */
    torn = 0;
    s0 = w->hdr->seq;
    if (s0 & 1u)
    {
        torn = 1;
    }
    else
    {
        __sync_synchronize();
        copy_w = new_w < old_w ? new_w : old_w;
        copy_h = new_h < old_h ? new_h : old_h;
        for (r = 0; r < copy_h; r++)
        {
            uint32_t *srow = (uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE +
                                          (size_t)r * (w->hdr->pitch / 4));
            uint32_t *drow = (uint32_t *)((uint8_t *)new_hdr + WM_HDR_SIZE +
                                          (size_t)r * new_w * 4);
            for (cc = 0; cc < copy_w; cc++)
                drow[cc] = srow[cc];
        }
        __sync_synchronize();
        s1 = w->hdr->seq;
        if (s0 != s1)
            torn = 1;
    }
    if (!torn)
    {
        /* Copy succeeded: clear any area beyond the old content. */
        for (r = copy_h; r < new_h; r++)
        {
            uint32_t *drow = (uint32_t *)((uint8_t *)new_hdr + WM_HDR_SIZE +
                                          (size_t)r * new_w * 4);
            for (cc = 0; cc < new_w; cc++)
                drow[cc] = 0;
        }
    }

    /* Free the old segment. */
    shm_destroy(w->shmid);

    /* Update window to point at the new segment. */
    w->shmid = new_id;
    w->hdr = new_hdr;
    w->w = new_w;
    w->h = new_h;
    w->hdr->ready = 1;
    return 0;
}

/* ---- cursor ---- */

static uint32_t comp_cursor_save[CURS_W * CURS_H];
static int comp_cursor_save_ok;
static int comp_cursor_x, comp_cursor_y;

static void comp_cursor_snapshot(struct comp_ctx *c, struct axgui_fb *fb, int x, int y)
{
    (void)c;
    int r, cc;
    if (!fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
        for (cc = 0; cc < CURS_W; cc++)
        {
            int px = x + cc, py = y + r;
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
                comp_cursor_save[(size_t)r * CURS_W + cc] = 0;
            else
                comp_cursor_save[(size_t)r * CURS_W + cc] =
                    fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px];
        }
    comp_cursor_x = x;
    comp_cursor_y = y;
    comp_cursor_save_ok = 1;
}

static void comp_cursor_restore(struct comp_ctx *c, struct axgui_fb *fb)
{
    (void)c;
    int r, cc;
    if (!comp_cursor_save_ok || !fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
        for (cc = 0; cc < CURS_W; cc++)
        {
            int px = comp_cursor_x + cc, py = comp_cursor_y + r;
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
                continue;
            fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px] =
                comp_cursor_save[(size_t)r * CURS_W + cc];
        }
}

static void comp_redraw_cursor_only(struct comp_ctx *c, struct axgui_fb *fb)
{
    int ux0, uy0, ux1, uy1, uw, uh;
    comp_cursor_restore(c, fb);
    ux0 = comp_cursor_x < c->mx ? comp_cursor_x : c->mx;
    uy0 = comp_cursor_y < c->my ? comp_cursor_y : c->my;
    ux1 = (comp_cursor_x + CURS_W > c->mx + CURS_W ? comp_cursor_x + CURS_W
                                                : c->mx + CURS_W);
    uy1 = (comp_cursor_y + CURS_H > c->my + CURS_H ? comp_cursor_y + CURS_H
                                                : c->my + CURS_H);
    uw = ux1 - ux0;
    uh = uy1 - uy0;
    comp_cursor_snapshot(c, fb, c->mx, c->my);
    axgui_cursor(fb, c->mx, c->my);
    if (axgui_present_rect(fb, ux0, uy0, uw, uh) < 0)
        axgui_present(fb);
}

/* ---- blit ---- */

static long comp_blit_content(struct axgui_fb *fb, struct comp_win *w)
{
    uint32_t *src;
    uint32_t s0, s1;
    int r;
    uint32_t src_stride;
    uint32_t copy_w, copy_h;
    if (!w->hdr || !w->hdr->ready || w->dead || w->closing || !w->hdr->ready)
        return -1;
    s0 = w->hdr->seq;
    if (s0 & 1u)
        return -1;
    src = (uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE);
    /* Source row stride is the physical buffer pitch (hdr->pitch), which is
       fixed at WM_SHM_MAX_W * 4 bytes. The logical window width/height
       (w->w/h->h) are clamped to WM_SHM_MAX_W/H in comp_do_resize(), so
       copying w->w x w->h columns/rows at hdr->pitch stride is always in
       bounds — no cropping during resize. */
    src_stride = w->hdr->pitch / 4;
    copy_w = w->w;
    copy_h = w->h;
    for (r = 0; r < (int)copy_h; r++)
    {
        uint32_t *drow =
            &fb->px[(size_t)(w->y + FRAME_TOP + r) * (fb->pitch / 4) +
                    (size_t)(w->x + FRAME_X)];
        uint32_t *srow = &src[(size_t)r * src_stride];
        int cc;
        if (w->y + FRAME_TOP + r < 0 ||
            w->y + FRAME_TOP + r >= (int)fb->h)
            continue;
        for (cc = 0; cc < (int)copy_w; cc++)
        {
            if (w->x + FRAME_X + cc < 0 ||
                w->x + FRAME_X + cc >= (int)fb->w)
                continue;
            drow[cc] = srow[cc];
        }
    }
    __sync_synchronize();
    s1 = w->hdr->seq;
    if (s0 != s1)
        return -1;
    return (long)s0;
}

/* ---- open / close / free ---- */

static int comp_build_client_cmd(char *cmd, size_t cap, const char *prog,
                                 long shmid, int evfd, uint32_t gen,
                                 const char *initial, long uid, long gid)
{
    char user[48];
    int r;
    user[0] = 0;
    if (uid >= 0 && gid >= 0)
    {
        int u = snprintf(user, sizeof(user), " --user %ld %ld", uid, gid);
        if (u < 0 || (size_t)u >= sizeof(user))
            return -1;
    }
    if (initial && initial[0])
        r = snprintf(cmd, cap, "%s --wm %ld %u %u %d %u%s -c %s", prog,
                     shmid, (unsigned)WM_SHM_MAX_W, (unsigned)WM_SHM_MAX_H,
                     evfd, gen, user, initial);
    else
        r = snprintf(cmd, cap, "%s --wm %ld %u %u %d %u%s", prog, shmid,
                     (unsigned)WM_SHM_MAX_W, (unsigned)WM_SHM_MAX_H,
                     evfd, gen, user);
    if (r < 0 || (size_t)r >= cap)
        return -1;
    return 0;
}

static struct comp_win *comp_open_client_in(struct comp_ctx *c,
                                            struct comp_win *w,
                                            const char *title,
                                            const char *prog,
                                            const char *initial,
                                            long uid, long gid)
{
    int fds[2];
    char cmd[224];
    long pid;
    int i;
    size_t pxn, k;
    if (!w || !w->used || w->pid > 0 || w->evfd >= 0 || !w->hdr)
        return 0;
    if (pipe(fds) < 0)
        return 0;
    if (++c->gencnt == 0 || c->gencnt > 0x7FFFFFFFu)
        c->gencnt = 1;
    pxn = (size_t)WM_SHM_MAX_W * WM_SHM_MAX_H;
    for (k = 0; k < pxn; k++)
        ((uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE))[k] = D_WinBG;
    w->hdr->magic = WM_MAGIC;
    w->hdr->w = w->w;
    w->hdr->h = w->h;
    w->hdr->closed = 0;
    w->hdr->seq = 0;
    w->hdr->ready = 0;
    w->hdr->gen = c->gencnt;
    w->hdr->gfx_caps = c->caps.caps;
    w->hdr->gfx_detail = c->caps.detail;
    w->hdr->pad = 0;
    w->hdr->pad2 = 0;
    w->hdr->pitch = (uint32_t)(WM_SHM_MAX_W * 4);
    if (comp_build_client_cmd(cmd, sizeof(cmd), prog, w->shmid, fds[0],
                              c->gencnt, initial, uid, gid) < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    pid = sys_spawn_cmd(cmd, strlen(cmd));
    close(fds[0]);
    if (pid < 0)
    {
        close(fds[1]);
        return 0;
    }
    printf("compositor: spawned '%s' pid=%ld evfd=%d shm=%ld gen=%u\n", prog,
           pid, fds[1], w->shmid, c->gencnt);
    memset(w->title, 0, sizeof(w->title));
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->live = 0;
    w->closing = 0;
    w->dead = 0;
    w->focused = 1;
    w->dragging = 0;
    w->resizing = 0;
    w->pid = pid;
    w->evfd = fds[1];
    w->status = 0;
    w->chain_login = 0;
    w->is_login = 0;
    w->gen = c->gencnt;
    w->ready_deadline = (long)time(0) + 4;
    w->close_deadline = 0;
    w->last_mx = -1;
    w->last_my = -1;
    w->last_btn = 0;
    w->seq = 0;
    {
        int kk = 0, jj;
        for (jj = 0; jj < c->nz; jj++)
        {
            if (&c->wins[c->z[jj]] != w)
                c->z[kk++] = c->z[jj];
        }
        c->z[kk++] = (int)(w - c->wins);
        c->nz = kk;
    }
    for (i = 0; i < WM_MAX_WIN; i++)
        if (&c->wins[i] != w)
            c->wins[i].focused = 0;
    return w;
}

static int comp_open_client(struct comp_ctx *c, const char *title,
                            const char *prog, const char *initial,
                            long uid, long gid,
                            int fullscreen, int noclose)
{
    struct comp_win *w = 0;
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
    {
        if (!c->wins[i].used && c->wins[i].shmid > 0 && c->wins[i].hdr)
        {
            w = &c->wins[i];
            break;
        }
    }
    if (!w)
        return 0;
    w->used = 1;
    w->w = WM_WIN_W;
    w->h = WM_WIN_H;
    w->fullscreen = fullscreen;
    w->noclose = noclose;
    if (w->fullscreen)
    {
        w->x = ((int)c->fb.w - (int)w->w) / 2;
        w->y = ((int)c->fb.h - (int)w->h) / 2;
    }
    else
    {
        w->x = 60 + (c->nz % 4) * 28;
        w->y = TOPBAR + 30 + (c->nz % 4) * 24;
    }
    if (w->x + (int)(w->w + FRAME_X * 2) > (int)c->fb.w - 8)
        w->x = (int)c->fb.w - 8 - (int)(w->w + FRAME_X * 2);
    if (w->x < 0)
        w->x = 0;
    if (w->y + (int)(FRAME_TOP + w->h + FRAME_BOT) > (int)c->fb.h - 8)
        w->y = (int)c->fb.h - 8 - (int)(FRAME_TOP + w->h + FRAME_BOT);
    if (w->y < TOPBAR)
        w->y = TOPBAR;
    if (!comp_open_client_in(c, w, title, prog, initial, uid, gid))
    {
        w->used = 0;
        return 0;
    }
    comp_win_focus(c, w);
    return (int)(w - c->wins);
}

static void comp_request_close(struct comp_ctx *c, struct comp_win *w, long now)
{
    if (!w->used || w->dead || w->pid <= 0 || w->noclose)
        return;
    if (w->hdr)
        w->hdr->closed = 1;
    if (w->evfd >= 0)
    {
        close(w->evfd);
        w->evfd = -1;
    }
    if (w->dragging || c->drag_win == (int)(w - c->wins))
    {
        w->dragging = 0;
        w->resizing = 0;
        if (c->drag_win == (int)(w - c->wins))
            c->drag_win = -1;
    }
    if (!w->closing)
    {
        w->closing = 1;
        w->close_deadline = now + 1;
    }
}

static void comp_free_slot(struct comp_ctx *c, struct comp_win *w)
{
    int i, k;
    if (w->pid > 0)
        return;
    w->used = 0;
    w->live = 0;
    w->closing = 0;
    w->dead = 0;
    w->dragging = 0;
    w->resizing = 0;
    w->fullscreen = 0;
    w->noclose = 0;
    w->chain_login = 0;
    w->is_login = 0;
    if (c->drag_win == (int)(w - c->wins))
        c->drag_win = -1;
    k = 0;
    for (i = 0; i < c->nz; i++)
    {
        if (&c->wins[c->z[i]] != w)
            c->z[k++] = c->z[i];
    }
    c->nz = k;
    comp_elect_focus(c);
}

static void comp_close_slot(struct comp_ctx *c, struct comp_win *w)
{
    int st = 0;
    if (w->pid > 0)
    {
        if (w->hdr)
            w->hdr->closed = 1;
        axgui_msleep(150);
        if (sys_waitpid_nb((int)w->pid, &st) != w->pid)
        {
            kill((int)w->pid, SIGKILL);
            sys_waitpid((int)w->pid, &st);
        }
        w->pid = -1;
        w->dead = 1;
        w->status = st;
        if (w->evfd >= 0)
        {
            close(w->evfd);
            w->evfd = -1;
        }
    }
    comp_free_slot(c, w);
}

/* ---- top bar / menu ---- */

static int comp_has_fullscreen(struct comp_ctx *c)
{
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
        if (c->wins[i].used && !c->wins[i].dead && c->wins[i].fullscreen)
            return 1;
    return 0;
}

static int comp_any_windows(struct comp_ctx *c)
{
    int i;
    for (i = 0; i < WM_MAX_WIN; i++)
        if (c->wins[i].used && !c->wins[i].dead)
            return 1;
    return 0;
}

static void comp_render_topbar(struct comp_ctx *c)
{
    long now = (long)time(0);
    char clock[32];
    int hh, mm, ss;
    axgui_fill(&c->fb, 0, 0, (int)c->fb.w, TOPBAR, D_BAR);
    axgui_fill(&c->fb, 4, 3, 92, 18, c->menu ? D_Accent : D_Title);
    axgui_text(&c->fb, "axiome", 12, 4, D_FG,
               c->menu ? D_Accent : D_Title);
    if (now < 0)
        now = 0;
    ss = (int)((now % 60 + 60) % 60);
    mm = (int)(((now / 60) % 60 + 60) % 60);
    hh = (int)(((now / 3600) % 24 + 24) % 24);
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d UTC", hh, mm, ss);
    axgui_text(&c->fb, clock,
               (int)c->fb.w / 2 - axgui_text_width(clock) / 2, 4, D_FG, D_BAR);
    axgui_fill(&c->fb, (int)c->fb.w - 120, 3, 56, 18, D_Title);
    axgui_text(&c->fb, "+Term", (int)c->fb.w - 112, 4, D_FG, D_Title);
}

static void comp_render_menu(struct comp_ctx *c)
{
    int mh = (5 + c->napps + 1) * FONT_HEIGHT + 8;
    int my = TOPBAR + 2;
    int row = 0;
    int i;
    axgui_fill(&c->fb, 4, my, 240, mh, D_Menu);
    axgui_rect(&c->fb, 4, my, 240, mh, D_Accent);
    axgui_text(&c->fb, "+ New terminal", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Prompt, D_Menu);
    axgui_text(&c->fb, "= System info", 12, my + 4 + row++ * FONT_HEIGHT,
               D_FG, D_Menu);
    axgui_text(&c->fb, "* gfx demo (fullscreen)", 12,
               my + 4 + row++ * FONT_HEIGHT, D_FG, D_Menu);
    axgui_text(&c->fb, "R Restart", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Prompt, D_Menu);
    axgui_text(&c->fb, "P Power off", 12, my + 4 + row++ * FONT_HEIGHT,
               D_Close, D_Menu);
    for (i = 0; i < c->napps; i++)
    {
        char entry[80];
        snprintf(entry, sizeof(entry), "  %s", c->apps[i]);
        axgui_text(&c->fb, entry, 12, my + 4 + row++ * FONT_HEIGHT, D_FG,
                   D_Menu);
    }
}

/* ---- resize ---- */

static int comp_resize_edge_at(struct comp_ctx *c, struct comp_win *w, int x, int y)
{
    (void)c;
    int frame_w = (int)(w->w + FRAME_X * 2);
    int frame_h = (int)(FRAME_TOP + w->h + FRAME_BOT);
    int in_title = y >= w->y && y < w->y + TITLEBAR;
    int left = x >= w->x && x < w->x + COMP_RESIZE_BORDER;
    int right = x >= w->x + frame_w - COMP_RESIZE_BORDER &&
                x < w->x + frame_w;
    int bottom = y >= w->y + frame_h - COMP_RESIZE_BORDER &&
                 y < w->y + frame_h;

    if (in_title)
        return COMP_RESIZE_NONE; /* title bar: drag or close, not resize */
    if (left && bottom)
        return COMP_RESIZE_BL;
    if (right && bottom)
        return COMP_RESIZE_BR;
    if (left)
        return COMP_RESIZE_LEFT;
    if (right)
        return COMP_RESIZE_RIGHT;
    if (bottom)
        return COMP_RESIZE_BOTTOM;
    return COMP_RESIZE_NONE;
}

static void comp_start_resize(struct comp_ctx *c, struct comp_win *w, int edge,
                              int x, int y)
{
    (void)c;
    w->resizing = 1;
    w->resize_edge = edge;
    w->resize_ox = x;
    w->resize_oy = y;
    w->resize_win_x = w->x;
    w->resize_win_y = w->y;
    w->resize_ow = w->w;
    w->resize_oh = w->h;
    c->drag_win = (int)(w - c->wins);
    w->dragging = 0;
}

static void comp_do_resize(struct comp_ctx *c, struct comp_win *w, int x, int y)
{
    (void)c;
    int dx = x - w->resize_ox;
    int dy = y - w->resize_oy;
    int new_w = (int)w->resize_ow;
    int new_h = (int)w->resize_oh;
    int new_x = w->resize_win_x;
    int new_y = w->resize_win_y;

    switch (w->resize_edge)
    {
        case COMP_RESIZE_RIGHT:
            new_w += dx;
            break;
        case COMP_RESIZE_LEFT:
            new_w -= dx;
            new_x += dx;
            break;
        case COMP_RESIZE_BOTTOM:
            new_h += dy;
            break;
        case COMP_RESIZE_TOP:
            new_h -= dy;
            new_y += dy;
            break;
        case COMP_RESIZE_TR:
            new_w += dx;
            new_h -= dy;
            new_y += dy;
            break;
        case COMP_RESIZE_TL:
            new_w -= dx;
            new_h -= dy;
            new_x += dx;
            new_y += dy;
            break;
        case COMP_RESIZE_BR:
            new_w += dx;
            new_h += dy;
            break;
        case COMP_RESIZE_BL:
            new_w -= dx;
            new_h += dy;
            new_x += dx;
            break;
        default:
            break;
    }
    if (new_w < (int)WM_SHM_MIN_W)
    {
        if (w->resize_edge == COMP_RESIZE_LEFT ||
            w->resize_edge == COMP_RESIZE_TL ||
            w->resize_edge == COMP_RESIZE_BL)
            new_x -= (int)WM_SHM_MIN_W - new_w;
        new_w = (int)WM_SHM_MIN_W;
    }
    if (new_h < (int)WM_SHM_MIN_H)
    {
        if (w->resize_edge == COMP_RESIZE_TOP ||
            w->resize_edge == COMP_RESIZE_TL ||
            w->resize_edge == COMP_RESIZE_TR)
            new_y -= (int)WM_SHM_MIN_H - new_h;
        new_h = (int)WM_SHM_MIN_H;
    }
    if (new_w > (int)WM_SHM_MAX_W)
        new_w = (int)WM_SHM_MAX_W;
    if (new_h > (int)WM_SHM_MAX_H)
        new_h = (int)WM_SHM_MAX_H;
    w->x = new_x;
    w->y = new_y;
    w->w = (uint32_t)new_w;
    w->h = (uint32_t)new_h;
    /* Do NOT touch hdr->w/h here: the client's pixel buffer is still at the
       old stride until it repaints after WM_EV_RESIZE. Blitting at hdr->w
       stride with clamped width keeps pixels aligned; new frame areas show
       stale pixels until the client repaints. */
}

static void comp_send_resize(struct comp_ctx *c, struct comp_win *w)
{
    if (!w->hdr || !w->hdr->ready || w->dead || w->closing || w->evfd < 0)
        return;
    struct wm_event re;
    re.type = WM_EV_RESIZE;
    re.code = 0; /* live resize: same SHM buffer, new logical dims only */
    re.x = (int32_t)w->w;
    re.y = (int32_t)w->h;
    axgui_wm_send(w->evfd, &re);
}

/* ---- click handling ---- */

static int comp_handle_click(struct comp_ctx *c, int x, int y, long now)
{
    if (y >= 0 && y < TOPBAR)
    {
        if (x >= 4 && x < 96)
        {
            c->menu = !c->menu;
            return 1;
        }
        if (x >= (int)c->fb.w - 120 && x < (int)c->fb.w - 64)
        {
            comp_open_client(c, "Terminal", "axterm", 0, c->sess_uid,
                             c->sess_gid, 0, 0);
            c->menu = 0;
            return 1;
        }
        return 1;
    }
    if (c->menu && x >= 4 && x < 244)
    {
        int idx = (y - TOPBAR - 4) / FONT_HEIGHT;
        int row = 0;
        int i;
        if (idx == row++)
        {
            comp_open_client(c, "Terminal", "axterm", 0, c->sess_uid,
                             c->sess_gid, 0, 0);
            c->menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            comp_open_client(c, "System info", "axinfo", 0, -1, -1, 0, 0);
            c->menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            c->menu = 0;
            if (c->gfx_pid <= 0)
            {
                long gp = sys_spawn_cmd("gfx_test", 8);
                if (gp > 0)
                {
                    c->gfx_pid = gp;
                    axgui_grab(c->input_fd, 0);
                }
            }
            return 1;
        }
        if (idx == row++)
        {
            c->menu = 0;
            sys_reboot();
            return 1;
        }
        if (idx == row++)
        {
            c->menu = 0;
            sys_poweroff();
            return 1;
        }
        for (i = 0; i < c->napps; i++)
        {
            if (idx == row++)
            {
                comp_open_client(c, c->apps[i], "axterm",
                                 c->apps[i], c->sess_uid, c->sess_gid, 0, 0);
                c->menu = 0;
                return 1;
            }
        }
        return 1;
    }
    c->menu = 0;
    {
        int wi = comp_win_at(c, x, y);
        if (wi < 0)
            return 0;
        struct comp_win *w = &c->wins[wi];
        if (!w->used)
            return 0;
        int fullscreen = w->fullscreen;
        int xbtn = !fullscreen &&
                   (x >= w->x + (int)w->w - TITLEBAR &&
                    x < w->x + (int)w->w - 4 &&
                    y >= w->y + 3 && y < w->y + TITLEBAR - 3);
        int resize_edge = comp_resize_edge_at(c, w, x, y);
        if (w->dead)
        {
            if (xbtn)
                comp_free_slot(c, w);
            return 1;
        }
        comp_win_focus(c, w);
        if (xbtn)
        {
            comp_request_close(c, w, now);
            return 1;
        }
        if (resize_edge != COMP_RESIZE_NONE && !fullscreen)
        {
            comp_start_resize(c, w, resize_edge, x, y);
            return 1;
        }
        if (!fullscreen && y >= w->y && y < w->y + TITLEBAR)
        {
            w->dragging = 1;
            w->drag_ox = x - w->x;
            w->drag_oy = y - w->y;
            c->drag_win = (int)(w - c->wins);
        }
        if (!w->closing && w->pid > 0 && w->evfd >= 0)
        {
            int lx, ly;
            if (fullscreen)
            {
                lx = x - w->x;
                ly = y - w->y;
            }
            else
            {
                lx = x - (w->x + FRAME_X);
                ly = y - (w->y + FRAME_TOP);
            }
            if (lx >= 0 && ly >= 0 && lx < (int)w->w &&
                ly < (int)w->h)
            {
                struct wm_event me;
                me.type = WM_EV_MOUSE;
                me.code = c->btn;
                me.x = lx;
                me.y = ly;
                if (axgui_wm_send(w->evfd, &me) == 0)
                {
                    w->last_mx = lx;
                    w->last_my = ly;
                    w->last_btn = c->btn;
                }
                else
                {
                    comp_request_close(c, w, now);
                }
            }
        }
        return 1;
    }
}

/* ---- motion / key dispatch ---- */

static void comp_dispatch_motion(struct comp_ctx *c, struct comp_win *hit,
                                 int lx, int ly, int x, int y)
{
    (void)x;
    (void)y;
    if (!hit || hit->dead || hit->closing || hit->pid <= 0 || hit->evfd < 0)
        return;
    if (lx >= 0 && ly >= 0 && lx < (int)hit->w && ly < (int)hit->h &&
        (lx != hit->last_mx || ly != hit->last_my || c->btn != hit->last_btn))
    {
        struct wm_event me;
        me.type = WM_EV_MOUSE;
        me.code = c->btn;
        me.x = lx;
        me.y = ly;
        if (axgui_wm_send(hit->evfd, &me) == 0)
        {
            hit->last_mx = lx;
            hit->last_my = ly;
            hit->last_btn = c->btn;
        }
        else
        {
            comp_request_close(c, hit, (long)time(0));
        }
    }
}

static void comp_dispatch_key(struct comp_ctx *c, struct comp_win *f,
                              uint32_t kc)
{
    if (kc == '`')
    {
        c->menu = !c->menu;
        return;
    }
    if (f && f->live && !f->closing && f->pid > 0 && f->evfd >= 0)
    {
        struct wm_event ke;
        ke.type = WM_EV_KEY;
        ke.code = kc;
        ke.x = 0;
        ke.y = 0;
        if (axgui_wm_send(f->evfd, &ke) < 0)
            comp_request_close(c, f, (long)time(0));
    }
}

/* ---- render ---- */

static void comp_render_frame(struct comp_ctx *c)
{
    int i, x, y;
    axgui_fill(&c->fb, 0, 0, (int)c->fb.w, (int)c->fb.h, D_BG);
    for (y = TOPBAR + 16; y < (int)c->fb.h; y += 32)
        for (x = 16; x < (int)c->fb.w; x += 32)
            axgui_px(&c->fb, x, y, D_BG2);

    for (i = 0; i < c->nz; i++)
    {
        struct comp_win *w = &c->wins[c->z[i]];
        uint32_t tc = w->focused ? D_TitleF : D_Title;
        if (w->fullscreen && !w->dead)
        {
            int cx = ((int)c->fb.w - (int)w->w) / 2;
            int cy = ((int)c->fb.h - (int)w->h) / 2;
            axgui_fill(&c->fb, 0, 0, (int)c->fb.w, (int)c->fb.h, D_WinBG);
            if (w->hdr && w->hdr->ready && !w->dead)
            {
                uint32_t *src;
                uint32_t s0, s1;
                int r, cc;
                s0 = w->hdr->seq;
                if (!(s0 & 1u))
                {
                    uint32_t src_stride = w->hdr->pitch / 4;
                    src = (uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE);
                    for (r = 0; r < (int)w->h; r++)
                    {
                        uint32_t *drow =
                            &c->fb.px[(size_t)(cy + r) * (c->fb.pitch / 4) +
                                      (size_t)(cx)];
                        uint32_t *srow = &src[(size_t)r * src_stride];
                        for (cc = 0; cc < (int)w->w; cc++)
                        {
                            if (cx + cc < 0 || cx + cc >= (int)c->fb.w ||
                                cy + r < 0 || cy + r >= (int)c->fb.h)
                                continue;
                            drow[cc] = srow[cc];
                        }
                    }
                    __sync_synchronize();
                    s1 = w->hdr->seq;
                    if (s0 == s1)
                        w->seq = s0;
                }
            }
            else
            {
                char note[64];
                axgui_fill(&c->fb, cx, cy, (int)w->w, (int)w->h,
                           D_WinBG);
                snprintf(note, sizeof(note), "starting %s ...", w->title);
                axgui_text(&c->fb, note, cx + 8, cy + 8, D_Dim, D_WinBG);
            }
            continue;
        }
        if (c->caps_ok && (c->caps.caps & AXGUI_CAP_SHADOWS))
            axgui_shadow(&c->fb, w->x, w->y, (int)(w->w + FRAME_X * 2),
                         (int)(FRAME_TOP + w->h + FRAME_BOT), 0x000000u, 60);
        axgui_fill(&c->fb, w->x, w->y,
                   (int)(w->w + FRAME_X * 2),
                   (int)(FRAME_TOP + w->h + FRAME_BOT), D_WinBG);
        axgui_fill(&c->fb, w->x, w->y, (int)(w->w + FRAME_X * 2), TITLEBAR,
                   tc);
        axgui_rect(&c->fb, w->x, w->y,
                   (int)(w->w + FRAME_X * 2),
                   (int)(FRAME_TOP + w->h + FRAME_BOT), D_Accent);
        axgui_text(&c->fb, w->title, w->x + 8, w->y + 2, D_FG, tc);
        axgui_fill(&c->fb, w->x + (int)w->w - TITLEBAR, w->y + 3, 14, 14,
                   D_Close);
        axgui_text(&c->fb, "x", w->x + (int)w->w - TITLEBAR + 3, w->y + 1,
                   D_FG, D_Close);
        if (w->hdr && w->hdr->ready && !w->dead)
        {
            long s = comp_blit_content(&c->fb, w);
            if (s >= 0)
                w->seq = (uint32_t)s;
        }
        else
        {
            char note[64];
            axgui_fill(&c->fb, w->x + FRAME_X, w->y + FRAME_TOP,
                       (int)w->w, (int)w->h, D_WinBG);
            if (w->dead)
                snprintf(note, sizeof(note), "exited (status %d) - X to dismiss",
                         w->status);
            else
                snprintf(note, sizeof(note), "starting %s ...", w->title);
            axgui_text(&c->fb, note, w->x + FRAME_X + 8,
                       w->y + FRAME_TOP + 8, D_Dim, D_WinBG);
        }
    }

    if (!comp_has_fullscreen(c))
    {
        comp_render_topbar(c);
        if (c->menu)
            comp_render_menu(c);
    }

    comp_cursor_snapshot(c, &c->fb, c->mx, c->my);
    axgui_cursor(&c->fb, c->mx, c->my);
}

#endif /* AXIOME_COMPOSITOR_H */
