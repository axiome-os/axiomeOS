/* axwm — mini window manager / desktop environment for axiomeOS.
   Fullscreen compositor on the Mesa winsys transport (/Devices/dri0 dumb
   buffer + PRESENT, software-rasterized like Gallium softpipe; see
   axgui.h). Pointer + keyboard come from /Devices/input0 while holding
   the GUI grab.

   Windows host REAL client programs from /Binaries (axterm, axinfo, ...):
   each window slot owns an SHM segment (see wm_abi.h) that the client
   attaches and draws into; input reaches the focused client as wm_events
   over a per-client pipe (fd 7). Child exits are reaped without stalling
   via SYS_WAITPID_NB; the X button asks (flag), then SIGKILLs and reaps.

   Desktop: top bar (launcher button, UTC clock, +Terminal, Exit),
   draggable fixed-size windows, launcher menu fed by readdir("/Binaries")
   (console programs open in an axterm client running them), plus a
   fullscreen gfx demo entry that temporarily hands over the scanout. */

#include "axgui.h"
#include "errno.h"
#include "signal.h"

#define TOPBAR 24
#define TITLEBAR 20
#define FRAME_X 8
#define FRAME_TOP (TITLEBAR + 6)
#define FRAME_BOT 6
#define WIN_FW (WM_WIN_W + FRAME_X * 2)
#define WIN_FH (WM_WIN_H + FRAME_TOP + FRAME_BOT)

/* Palette. */
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

struct win {
    int used;
    int x, y; /* frame origin; size fixed (WIN_FW x WIN_FH) */
    char title[48];
    int focused;
    int dragging;
    int drag_ox, drag_oy;
    long shmid;
    struct wm_win_hdr *hdr; /* compositor mapping of the window SHM */
    long pid;               /* client pid, -1 when none/reaped */
    int evfd;               /* event pipe write end, -1 when closed */
    int dead;               /* client reaped; frame kept until dismissed */
    int status;
    int last_mx, last_my;   /* last mouse state forwarded */
    uint32_t last_btn;
};

static struct win g_wins[WM_MAX_WIN];
static int g_z[WM_MAX_WIN];
static int g_nz;
static int g_menu;
static int g_exit_req;
static int g_mx, g_my;
static uint32_t g_btn;
static uint32_t g_prev_btn;
static int g_drag_win = -1;
static char g_apps[16][64];
static int g_napps;

#define CURS_W 16
#define CURS_H 16
static uint32_t g_cursor_save[CURS_W * CURS_H];
static int g_cursor_save_ok;
static int g_cursor_x = -1, g_cursor_y = -1;

static void win_close_slot(struct win *w);

static struct win *win_at(int x, int y)
{
    int i;
    for (i = g_nz - 1; i >= 0; i--)
    {
        struct win *w = &g_wins[g_z[i]];
        if (x >= w->x && x < w->x + (int)WIN_FW && y >= w->y &&
            y < w->y + (int)WIN_FH)
            return w;
    }
    return 0;
}

static void win_focus(struct win *w)
{
    int i, k;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        g_wins[i].focused = (&g_wins[i] == w);
    k = 0;
    for (i = 0; i < g_nz; i++)
    {
        if (&g_wins[g_z[i]] != w)
            g_z[k++] = g_z[i];
    }
    g_z[k++] = (int)(w - g_wins);
}

static struct win *focused_win(void)
{
    int i;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        if (g_wins[i].used && g_wins[i].focused)
            return &g_wins[i];
    return 0;
}

/* Reap a live client without stalling: flag, grace, SIGKILL, blocking reap
   of the (now certainly dying) child. */
static void client_stop(struct win *w)
{
    int st = 0;
    if (w->pid <= 0)
        return;
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

static void win_close_slot(struct win *w)
{
    int i, k;
    client_stop(w);
    w->used = 0;
    w->dragging = 0;
    k = 0;
    for (i = 0; i < g_nz; i++)
    {
        if (&g_wins[g_z[i]] != w)
            g_z[k++] = g_z[i];
    }
    g_nz = k;
}

/* Launch a client program into a free window slot. `prog` must speak the
   wm_abi client protocol (argv: --wm <shmid> <w> <h> [-c ...]). */
static struct win *open_client(const char *title, const char *prog,
                               const char *initial, struct axgui_fb *fb)
{
    struct win *w = 0;
    int fds[2];
    char cmd[160];
    long pid;
    int i;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
    {
        if (!g_wins[i].used)
        {
            w = &g_wins[i];
            break;
        }
    }
    if (!w || w->shmid <= 0)
        return 0;
    if (pipe(fds) < 0)
        return 0;
    /* The read end travels by inheritance; its number goes on the command
       line (no dup2 onto a fixed fd: the target might be one of the pipe's
       own ends, and dup2 would close it first). */
    if (initial && initial[0])
        snprintf(cmd, sizeof(cmd), "%s --wm %ld %u %u %d -c %s", prog,
                 w->shmid, WM_WIN_W, WM_WIN_H, fds[0], initial);
    else
        snprintf(cmd, sizeof(cmd), "%s --wm %ld %u %u %d", prog, w->shmid,
                 WM_WIN_W, WM_WIN_H, fds[0]);
    pid = sys_spawn_cmd(cmd, strlen(cmd));
    close(fds[0]); /* compositor's alias; the child keeps its own */
    if (pid < 0)
    {
        close(fds[1]);
        return 0;
    }
    printf("axwm: spawned '%s' pid=%ld evfd=%d shm=%ld\n", prog, pid,
           fds[1], w->shmid);

    memset(w->title, 0, sizeof(w->title));
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->x = 60 + (g_nz % 4) * 28;
    w->y = TOPBAR + 30 + (g_nz % 4) * 24;
    if (w->x + (int)WIN_FW > (int)fb->w - 8)
        w->x = (int)fb->w - 8 - (int)WIN_FW;
    if (w->x < 0)
        w->x = 0;
    if (w->y + (int)WIN_FH > (int)fb->h - 8)
        w->y = (int)fb->h - 8 - (int)WIN_FH;
    if (w->y < TOPBAR)
        w->y = TOPBAR;
    w->used = 1;
    w->focused = 1;
    w->dragging = 0;
    w->pid = pid;
    w->evfd = fds[1];
    w->dead = 0;
    w->status = 0;
    w->last_mx = -1;
    w->last_my = -1;
    w->last_btn = 0;
    /* Fresh pixels + header for the new client. */
    if (w->hdr)
    {
        size_t pxn = (size_t)WM_WIN_W * WM_WIN_H;
        size_t i2;
        for (i2 = 0; i2 < pxn; i2++)
            ((uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE))[i2] = D_WinBG;
        w->hdr->magic = WM_MAGIC;
        w->hdr->w = WM_WIN_W;
        w->hdr->h = WM_WIN_H;
        w->hdr->closed = 0;
        w->hdr->seq = 0;
        w->hdr->ready = 0;
    }
    g_z[g_nz++] = (int)(w - g_wins);
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        if (&g_wins[i] != w)
            g_wins[i].focused = 0;
    return w;
}

static void open_terminal(const char *cmd, struct axgui_fb *fb)
{
    struct win *w = open_client(cmd && cmd[0] ? cmd : "Terminal", "axterm",
                                cmd, fb);
    if (!w)
        printf("axwm: cannot launch axterm\n");
}

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

/* Fresh left-press. Returns 1 if consumed. */
static int handle_click(int x, int y, struct axgui_fb *fb, int input_fd)
{
    if (y >= 0 && y < TOPBAR)
    {
        if (x >= 4 && x < 96)
        {
            g_menu = !g_menu;
            return 1;
        }
        if (x >= (int)fb->w - 120 && x < (int)fb->w - 64)
        {
            open_terminal(0, fb);
            g_menu = 0;
            return 1;
        }
        if (x >= (int)fb->w - 60 && x < (int)fb->w - 4)
        {
            g_exit_req = 1;
            return 1;
        }
        return 1;
    }
    if (g_menu && x >= 4 && x < 244)
    {
        int idx = (y - TOPBAR - 4) / FONT_HEIGHT;
        int row = 0;
        int i;
        if (idx == row++)
        {
            open_terminal(0, fb);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            open_client("System info", "axinfo", 0, fb);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            g_menu = 0;
            axgui_grab(input_fd, 0);
            axgui_run_wait("gfx_test");
            axgui_grab(input_fd, 1);
            return 1;
        }
        for (i = 0; i < g_napps; i++)
        {
            if (idx == row++)
            {
                open_terminal(g_apps[i], fb);
                g_menu = 0;
                return 1;
            }
        }
        return 1;
    }
    g_menu = 0;
    {
        struct win *w = win_at(x, y);
        if (!w)
            return 0;
        win_focus(w);
        if (x >= w->x + (int)WIN_FW - TITLEBAR && x < w->x + (int)WIN_FW - 4 &&
            y >= w->y + 3 && y < w->y + TITLEBAR - 3)
        {
            win_close_slot(w);
            return 1;
        }
        if (y >= w->y && y < w->y + TITLEBAR)
        {
            w->dragging = 1;
            w->drag_ox = x - w->x;
            w->drag_oy = y - w->y;
            g_drag_win = (int)(w - g_wins);
        }
        return 1;
    }
}

static void blit_content(struct axgui_fb *fb, struct win *w)
{
    uint32_t *src;
    int r;
    if (!w->hdr || !w->hdr->ready || w->dead)
        return;
    src = (uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE);
    for (r = 0; r < (int)WM_WIN_H; r++)
    {
        uint32_t *drow =
            &fb->px[(size_t)(w->y + FRAME_TOP + r) * (fb->pitch / 4) +
                    (size_t)(w->x + FRAME_X)];
        uint32_t *srow = &src[(size_t)r * WM_WIN_W];
        int c;
        if (w->y + FRAME_TOP + r < 0 ||
            w->y + FRAME_TOP + r >= (int)fb->h)
            continue;
        for (c = 0; c < (int)WM_WIN_W; c++)
        {
            if (w->x + FRAME_X + c < 0 ||
                w->x + FRAME_X + c >= (int)fb->w)
                continue;
            drow[c] = srow[c];
        }
    }
}

static void cursor_snapshot(struct axgui_fb *fb, int x, int y)
{
    int r, c;
    if (x < 0 || y < 0 || x + CURS_W > (int)fb->w || y + CURS_H > (int)fb->h)
    {
        g_cursor_save_ok = 0;
        return;
    }
    for (r = 0; r < CURS_H; r++)
        for (c = 0; c < CURS_W; c++)
        {
            int px = x + c, py = y + r;
            if (px < 0 || py < 0 || px >= (int)fb->w || py >= (int)fb->h)
                g_cursor_save[(size_t)r * CURS_W + c] = 0;
            else
                g_cursor_save[(size_t)r * CURS_W + c] =
                    fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px];
        }
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_save_ok = 1;
}

static void cursor_restore(struct axgui_fb *fb)
{
    int r, c;
    if (!g_cursor_save_ok || !fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
        for (c = 0; c < CURS_W; c++)
        {
            int px = g_cursor_x + c, py = g_cursor_y + r;
            if (px < 0 || py < 0 || px >= (int)fb->w || py >= (int)fb->h)
                continue;
            fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px] =
                g_cursor_save[(size_t)r * CURS_W + c];
        }
}

static void redraw_cursor_only(struct axgui_fb *fb)
{
    int ux0, uy0, ux1, uy1, uw, uh;
    cursor_restore(fb);
    ux0 = g_cursor_x < g_mx ? g_cursor_x : g_mx;
    uy0 = g_cursor_y < g_my ? g_cursor_y : g_my;
    ux1 = (g_cursor_x + CURS_W > g_mx + CURS_W ? g_cursor_x + CURS_W
                                                   : g_mx + CURS_W);
    uy1 = (g_cursor_y + CURS_H > g_my + CURS_H ? g_cursor_y + CURS_H
                                                   : g_my + CURS_H);
    uw = ux1 - ux0;
    uh = uy1 - uy0;
    cursor_snapshot(fb, g_mx, g_my);
    axgui_cursor(fb, g_mx, g_my);
    if (axgui_present_rect(fb, ux0, uy0, uw, uh) < 0)
        axgui_present(fb);
}

static void render(struct axgui_fb *fb, int tick)
{
    int i, x, y;
    char clock[32];
    (void)tick;
    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, D_BG);
    for (y = TOPBAR + 16; y < (int)fb->h; y += 32)
        for (x = 16; x < (int)fb->w; x += 32)
            axgui_px(fb, x, y, D_BG2);

    for (i = 0; i < g_nz; i++)
    {
        struct win *w = &g_wins[g_z[i]];
        uint32_t tc = w->focused ? D_TitleF : D_Title;
        axgui_fill(fb, w->x, w->y, (int)WIN_FW, (int)WIN_FH, D_WinBG);
        axgui_fill(fb, w->x, w->y, (int)WIN_FW, TITLEBAR, tc);
        axgui_rect(fb, w->x, w->y, (int)WIN_FW, (int)WIN_FH, D_Accent);
        axgui_text(fb, w->title, w->x + 8, w->y + 2, D_FG, tc);
        axgui_fill(fb, w->x + (int)WIN_FW - TITLEBAR, w->y + 3, 14, 14,
                   D_Close);
        axgui_text(fb, "x", w->x + (int)WIN_FW - TITLEBAR + 3, w->y + 1,
                   D_FG, D_Close);
        if (w->hdr && w->hdr->ready && !w->dead)
        {
            blit_content(fb, w);
        }
        else
        {
            char note[64];
            axgui_fill(fb, w->x + FRAME_X, w->y + FRAME_TOP, (int)WM_WIN_W,
                       (int)WM_WIN_H, D_WinBG);
            if (w->dead)
                snprintf(note, sizeof(note), "exited (status %d) - X to dismiss",
                         w->status);
            else
                snprintf(note, sizeof(note), "starting %s ...", w->title);
            axgui_text(fb, note, w->x + FRAME_X + 8, w->y + FRAME_TOP + 8,
                       D_Dim, D_WinBG);
        }
    }

    axgui_fill(fb, 0, 0, (int)fb->w, TOPBAR, D_BAR);
    axgui_fill(fb, 4, 3, 92, 18, g_menu ? D_Accent : D_Title);
    axgui_text(fb, "axiome", 12, 4, D_FG, g_menu ? D_Accent : D_Title);
    {
        long now = (long)time(0);
        int hh, mm, ss;
        if (now < 0)
            now = 0;
        ss = (int)((now % 60 + 60) % 60);
        mm = (int)(((now / 60) % 60 + 60) % 60);
        hh = (int)(((now / 3600) % 24 + 24) % 24);
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d UTC", hh, mm, ss);
    }
    axgui_text(fb, clock,
               (int)fb->w / 2 - axgui_text_width(clock) / 2, 4, D_FG, D_BAR);
    axgui_fill(fb, (int)fb->w - 120, 3, 56, 18, D_Title);
    axgui_text(fb, "+Term", (int)fb->w - 112, 4, D_FG, D_Title);
    axgui_fill(fb, (int)fb->w - 60, 3, 56, 18, D_Close);
    axgui_text(fb, "Exit", (int)fb->w - 50, 4, D_FG, D_Close);

    if (g_menu)
    {
        int mh = (3 + g_napps + 1) * FONT_HEIGHT + 8;
        int my = TOPBAR + 2;
        int row = 0;
        axgui_fill(fb, 4, my, 240, mh, D_Menu);
        axgui_rect(fb, 4, my, 240, mh, D_Accent);
        axgui_text(fb, "+ New terminal", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_Prompt, D_Menu);
        axgui_text(fb, "= System info", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_FG, D_Menu);
        axgui_text(fb, "* gfx demo (fullscreen)", 12,
                   my + 4 + row++ * FONT_HEIGHT, D_FG, D_Menu);
        for (i = 0; i < g_napps; i++)
        {
            char entry[80];
            snprintf(entry, sizeof(entry), "  %s", g_apps[i]);
            axgui_text(fb, entry, 12, my + 4 + row++ * FONT_HEIGHT, D_FG,
                       D_Menu);
        }
    }

}

int main(int argc, char **argv)
{
    struct axgui_fb fb;
    struct axinput_event ev[64];
    int input_fd;
    int tick = 0;
    size_t win_bytes;
    int i;
    (void)argc;
    (void)argv;

    fb.fd = -1;
    if (axgui_dri_open(&fb) < 0)
    {
        printf("axwm: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    input_fd = axgui_input_open();
    if (input_fd < 0)
    {
        printf("axwm: no input device (need input0)\n");
        axgui_close(&fb);
        return 1;
    }
    axgui_grab(input_fd, 1);
    axgui_poll(input_fd, ev, 64);

    /* Fixed SHM pool: one window segment per slot, owned by the compositor
       for its whole lifetime (no per-client churn, nothing to free). */
    memset(g_wins, 0, sizeof(g_wins));
    win_bytes = WM_HDR_SIZE + (size_t)WM_WIN_W * WM_WIN_H * 4;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
    {
        long id;
        uint8_t *base;
        size_t k;
        g_wins[i].pid = -1;
        g_wins[i].evfd = -1;
        id = shm_create(win_bytes);
        if (id <= 0)
        {
            printf("axwm: shm_create failed for slot %d\n", i);
            continue;
        }
        base = (uint8_t *)shm_attach(id);
        if (!base)
        {
            printf("axwm: shm_attach failed for slot %d\n", i);
            continue;
        }
        for (k = 0; k < win_bytes; k++)
            base[k] = 0;
        g_wins[i].shmid = id;
        g_wins[i].hdr = (struct wm_win_hdr *)base;
    }

    g_mx = (int)fb.w / 2;
    g_my = (int)fb.h / 2;
    scan_apps();
    open_terminal(0, &fb);

    for (;;)
    {
        int n = axgui_poll(input_fd, ev, 64);
        int state_changed = 0;
        int mouse_moved = 0;
        for (i = 0; i < n; i++)
        {
            if (ev[i].type == AXINPUT_TYPE_MOUSE)
            {
                struct win *hit;
                g_mx += ev[i].dx;
                g_my += ev[i].dy;
                if (g_mx < 0)
                    g_mx = 0;
                if (g_my < 0)
                    g_my = 0;
                if (g_mx >= (int)fb.w)
                    g_mx = (int)fb.w - 1;
                if (g_my >= (int)fb.h)
                    g_my = (int)fb.h - 1;
                mouse_moved = (ev[i].dx || ev[i].dy);
                g_btn = ev[i].code;
                /* Forward motion to the window under the pointer. */
                hit = win_at(g_mx, g_my);
                if (hit && hit->pid > 0 && !hit->dead && hit->evfd >= 0 &&
                    hit == focused_win())
                {
                    int lx = g_mx - (hit->x + FRAME_X);
                    int ly = g_my - (hit->y + FRAME_TOP);
                    if (lx >= 0 && ly >= 0 && lx < (int)WM_WIN_W &&
                        ly < (int)WM_WIN_H &&
                        (lx != hit->last_mx || ly != hit->last_my ||
                         g_btn != hit->last_btn))
                    {
                        struct wm_event me;
                        me.type = WM_EV_MOUSE;
                        me.code = g_btn;
                        me.x = lx;
                        me.y = ly;
                        if (axgui_wm_send(hit->evfd, &me) == 0)
                        {
                            hit->last_mx = lx;
                            hit->last_my = ly;
                            hit->last_btn = g_btn;
                        }
                    }
                }
            }
            else if (ev[i].type == AXINPUT_TYPE_KEY)
            {
                uint32_t kc = ev[i].code;
                if (kc == '`')
                {
                    g_menu = !g_menu;
                }
                else
                {
                    struct win *f = focused_win();
                    if (f && f->pid > 0 && !f->dead && f->evfd >= 0)
                    {
                        struct wm_event ke;
                        ke.type = WM_EV_KEY;
                        ke.code = kc;
                        ke.x = 0;
                        ke.y = 0;
                        axgui_wm_send(f->evfd, &ke);
                    }
                    state_changed = 1;
                }
            }
        }
        if ((g_btn & AXINPUT_BTN_LEFT) && !(g_prev_btn & AXINPUT_BTN_LEFT))
        {
            if (handle_click(g_mx, g_my, &fb, input_fd))
                state_changed = 1;
        }
        if (!(g_btn & AXINPUT_BTN_LEFT) && (g_prev_btn & AXINPUT_BTN_LEFT))
        {
            if (g_drag_win >= 0 && g_drag_win < (int)WM_MAX_WIN)
                g_wins[g_drag_win].dragging = 0;
            g_drag_win = -1;
        }
        if ((g_btn & AXINPUT_BTN_LEFT) && g_drag_win >= 0)
        {
            struct win *w = &g_wins[g_drag_win];
            if (w->used && w->dragging)
            {
                w->x = g_mx - w->drag_ox;
                w->y = g_my - w->drag_oy;
                if (w->y < TOPBAR)
                    w->y = TOPBAR;
                if (w->x < -((int)WIN_FW - 60))
                    w->x = -((int)WIN_FW - 60);
                if (w->x > (int)fb.w - 40)
                    w->x = (int)fb.w - 40;
                if (w->y > (int)fb.h - 40)
                    w->y = (int)fb.h - 40;
                state_changed = 1;
            }
        }
        g_prev_btn = g_btn;

        /* Reap exited clients without stalling the frame loop.
           Clean exit (status 0) closes instantly; crashes keep a dead
           frame until dismissed, matching guixd behaviour. */
        for (i = 0; i < (int)WM_MAX_WIN; i++)
        {
            struct win *w = &g_wins[i];
            if (w->used && w->pid > 0)
            {
                int st = 0;
                long r = sys_waitpid_nb((int)w->pid, &st);
                if (r == w->pid)
                {
                    if (w->evfd >= 0)
                    {
                        close(w->evfd);
                        w->evfd = -1;
                    }
                    w->pid = -1;
                    if (st == 0)
                    {
                        int k, j;
                        w->used = 0;
                        w->dead = 0;
                        w->dragging = 0;
                        if (g_drag_win == (int)(w - g_wins))
                            g_drag_win = -1;
                        k = 0;
                        for (j = 0; j < g_nz; j++)
                        {
                            if (&g_wins[g_z[j]] != w)
                                g_z[k++] = g_z[j];
                        }
                        g_nz = k;
                        /* Re-elect focus so input does not stick on freed slot. */
                        for (j = 0; j < (int)WM_MAX_WIN; j++)
                            g_wins[j].focused = 0;
                        for (j = g_nz - 1; j >= 0; j--)
                        {
                            struct win *t = &g_wins[g_z[j]];
                            if (t->used && !t->dead)
                            {
                                t->focused = 1;
                                break;
                            }
                        }
                    }
                    else
                    {
                        w->dead = 1;
                        w->status = st;
                    }
                }
            }
        }

        if (g_exit_req)
            break;
        if (g_nz == 0)
            open_terminal(0, &fb);
        if (state_changed || g_cursor_x < 0)
        {
            render(&fb, tick++);
            axgui_present(&fb);
            cursor_snapshot(&fb, g_mx, g_my);
        }
        else if (mouse_moved)
        {
            redraw_cursor_only(&fb);
        }
        axgui_msleep(16);
    }

    for (i = 0; i < (int)WM_MAX_WIN; i++)
        if (g_wins[i].used)
            win_close_slot(&g_wins[i]);
    axgui_grab(input_fd, 0);
    close(input_fd);
    axgui_close(&fb);
    printf("\n[axwm closed, back to console]\n");
    return 0;
}
