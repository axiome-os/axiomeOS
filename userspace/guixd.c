/* guixd — axiome display server daemon ("GUI X daemon").
   Runs as a system service spawned by axiome-init, so it inherits the
   SYSTEM role and is one of the very few processes allowed to talk to the
   graphics/input devices directly (/Devices/dri0 + /Devices/input0; the
   kernel gates them to ROLE_SYSTEM in sys_open). Regular users get the
   desktop through this daemon instead of raw hardware access.

   Builds on the shared compositor core (userspace/compositor.h): window
   lifecycle, SHM pool, seqlock blit, cursor snapshot/restore, top bar,
   menu, input dispatch, and resize. Guixd-specific behaviour — session
   management (login -> terminal handoff), first-boot setup chaining,
   adaptive frame pacing, async fullscreen demo, empty-desktop auto-reopen
   — stays here.

   Render loop: full software render + PRESENT only when state changes.
   Pointer-only moves use cursor snapshot + present_rect. */

#define TOPBAR 24
#define TITLEBAR 20
#define FRAME_X 8
#define FRAME_TOP (TITLEBAR + 6)
#define FRAME_BOT 6

#define CURS_W 16
#define CURS_H 16

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

#include "compositor.h"
#include "errno.h"
#include "signal.h"

/* Forward declaration. */
static int read_whole_file(const char *path, char *buf, size_t cap);

/* ---- guixd-specific session management ---- */

/* True when /etc/passwd already holds a "user" or "admin" account. */
static int has_regular_user(void)
{
    static char text[8193];
    char *line;
    if (read_whole_file("/etc/passwd", text, sizeof(text)) < 0)
        return 0;
    line = text;
    while (*line)
    {
        char *nl = line;
        char *f[7];
        int nf = 0;
        char *p = line;
        while (*nl && *nl != '\n')
            nl++;
        if (*nl)
            *nl++ = 0;
        while (*p && nf < 7)
        {
            f[nf++] = p;
            while (*p && *p != ':')
                p++;
            if (*p)
                *p++ = 0;
        }
        if (nf >= 7 && f[4][0] &&
            (strcmp(f[4], "user") == 0 || strcmp(f[4], "admin") == 0))
            return 1;
        line = nl;
    }
    return 0;
}

/* Loop-read a whole (small) file; NUL-terminated, truncated at cap-1. */
static int read_whole_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    size_t got = 0;
    long r;
    if (fd < 0 || !buf || cap == 0)
        return -1;
    while (got + 1 < cap)
    {
        r = read(fd, buf + got, cap - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    return 0;
}

#define GUIXD_SESS_FMT "/var/run/axlogin.%ld"

/* Consume the session file left by axlogin: "<uid> <gid>\n". Unlinked
   even when unparsable so nothing stale survives. */
static int consume_session_file(long shmid, long *uid, long *gid)
{
    char path[64];
    char body[64];
    const char *p;
    long v;
    int fd;
    size_t got = 0;
    long r;
    if (shmid <= 0 || !uid || !gid)
        return -1;
    snprintf(path, sizeof(path), GUIXD_SESS_FMT, shmid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (got + 1 < sizeof(body))
    {
        r = read(fd, body + got, sizeof(body) - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    unlink(path);
    body[got] = 0;
    p = body;
    v = 0;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (*p - '0');
        if (v > 0x7FFFFFFFL)
            return -1;
        p++;
    }
    if (*p != ' ')
        return -1;
    *uid = v;
    p++;
    v = 0;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (*p - '0');
        if (v > 0x7FFFFFFFL)
            return -1;
        p++;
    }
    if (*p != '\n' && *p != 0)
        return -1;
    *gid = v;
    return 0;
}

/* Replace a reaped login window with its authenticated session in the
   same slot. Records the session user; later terminals inherit it. */
static int win_reopen_session(struct comp_ctx *c, struct comp_win *w,
                              long uid, long gid)
{
    if (!w || !w->used || w->pid > 0 || w->dead)
        return 0;
    w->status = 0;
    w->chain_login = 0;
    w->is_login = 0;
    w->fullscreen = 0;
    w->noclose = 0;
    if (!comp_open_client_in(c, w, "Terminal", "axterm", 0, uid, gid))
    {
        w->dead = 1;
        w->status = -1;
        comp_elect_focus(c);
        return 0;
    }
    c->sess_uid = uid;
    c->sess_gid = gid;
    return 1;
}

/* Scan /Binaries for launcher menu entries. */
static void scan_apps(struct comp_ctx *c)
{
    struct vfs_dirent ents[40];
    int n, i;
    c->napps = 0;
    n = readdir("/Binaries", ents, 40);
    if (n <= 0)
        return;
    for (i = 0; i < n && c->napps < 16; i++)
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
        strncpy(c->apps[c->napps], nm, 63);
        c->apps[c->napps][63] = 0;
        c->napps++;
    }
}

/* First window(s) at boot: first-boot setup when no account exists yet
   (chained into login on success), otherwise the login window directly. */
static void boot_session(struct comp_ctx *c)
{
    if (has_regular_user())
    {
        comp_open_client(c, "Login", "axlogin", 0, -1, -1, 1, 1);
        return;
    }
    comp_open_client(c, "First-boot setup", "axoobe", 0, -1, -1, 1, 1);
    c->wins[0].chain_login = 1; /* the first slot is the setup window */
}

int main(int argc, char **argv)
{
    struct comp_ctx c;
    struct axinput_event ev[64];
    long last_clock = -1;
    (void)argc;
    (void)argv;

    memset(&c, 0, sizeof(c));
    c.fb.fd = -1;
    if (axgui_dri_open(&c.fb) < 0)
    {
        printf("guixd: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    c.input_fd = axgui_input_open();
    if (c.input_fd < 0)
    {
        printf("guixd: no input device (need input0)\n");
        axgui_close(&c.fb);
        return 1;
    }
    axgui_grab(c.input_fd, 1);
    axgui_poll(c.input_fd, ev, 64);

    if (axgui_get_caps(&c.fb, &c.caps) == 0)
    {
        c.caps_ok = 1;
        printf("guixd: gfx caps 0x%lx detail=%s\n",
               (unsigned long)c.caps.caps,
               c.caps.detail ? "detailed" : "simplified");
        if (!(c.caps.caps & AXGUI_CAP_BLUR))
            printf("guixd: blur disabled (CPU fallback gated)\n");
    }
    else
    {
        printf("guixd: gfx caps query failed, assuming simplified\n");
        c.caps.caps = 0;
        c.caps.detail = 0;
    }
    printf("guixd: display server up (%ux%u) mode=%s\n", c.fb.w, c.fb.h,
           c.caps.detail ? "detailed" : "simplified");

    comp_shm_pool_create(&c);
    scan_apps(&c);
    boot_session(&c);

    uint64_t avg_us = AXGUI_TARGET_60_US;
    uint64_t frame_id = 0;

    for (;;)
    {
        uint64_t frame_start_us = axgui_now_us();
        long now = (long)time(0);
        int state_changed = 0;
        int mouse_moved = 0;
        int motion_pending = 0;
        int drained;

        for (drained = 0; drained < 4; drained++)
        {
            int n = axgui_poll(c.input_fd, ev, 64);
            if (n <= 0)
                break;
            for (int i = 0; i < n; i++)
            {
                if (ev[i].type == AXINPUT_TYPE_MOUSE)
                {
                    uint32_t pressed, released;
                    int nx = c.mx + ev[i].dx;
                    int ny = c.my + ev[i].dy;
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx >= (int)c.fb.w) nx = (int)c.fb.w - 1;
                    if (ny >= (int)c.fb.h) ny = (int)c.fb.h - 1;
                    if (nx != c.mx || ny != c.my)
                        mouse_moved = 1;
                    c.mx = nx;
                    c.my = ny;
                    pressed = ev[i].code & ~c.btn;
                    released = ~ev[i].code & c.btn;
                    c.btn = ev[i].code;
                    if (c.btn & AXINPUT_BTN_LEFT)
                        state_changed = 1;
                    motion_pending = 1;
                    if (pressed & AXINPUT_BTN_LEFT)
                    {
                        if (comp_handle_click(&c, c.mx, c.my, now))
                            state_changed = 1;
                    }
                    if (released & AXINPUT_BTN_LEFT)
                    {
                        if (c.drag_win >= 0 &&
                            c.drag_win < WM_MAX_WIN)
                        {
                            struct comp_win *rw = &c.wins[c.drag_win];
                            int was_resizing = rw->resizing;
                            rw->dragging = 0;
                            rw->resizing = 0;
                            if (was_resizing)
                            {
                                comp_send_resize(&c, rw);
                            }
                        }
                        c.drag_win = -1;
                        state_changed = 1;
                    }
                }
                else if (ev[i].type == AXINPUT_TYPE_KEY)
                {
                    uint32_t kc = ev[i].code;
                    struct comp_win *f = comp_focused_win(&c);
                    comp_dispatch_key(&c, f, kc);
                    state_changed = 1;
                }
            }
        }

        /* Drag / resize while left button held. */
        if ((c.btn & AXINPUT_BTN_LEFT) && c.drag_win >= 0)
        {
            struct comp_win *w = &c.wins[c.drag_win];
            if (w->used && w->dragging)
            {
                w->x = c.mx - w->drag_ox;
                w->y = c.my - w->drag_oy;
                if (w->y < TOPBAR)
                    w->y = TOPBAR;
                if (w->x < -((int)(w->w + FRAME_X * 2) - 60))
                    w->x = -((int)(w->w + FRAME_X * 2) - 60);
                if (w->x > (int)c.fb.w - 40)
                    w->x = (int)c.fb.w - 40;
                if (w->y > (int)c.fb.h - 40)
                    w->y = (int)c.fb.h - 40;
                state_changed = 1;
            }
            else if (w->used && w->resizing)
            {
                comp_do_resize(&c, w, c.mx, c.my);
                comp_send_resize(&c, w);
                state_changed = 1;
            }
        }

        /* Coalesced motion to the window under the pointer. */
        if (motion_pending)
        {
            int wi = comp_win_at(&c, c.mx, c.my);
            if (wi >= 0)
            {
                struct comp_win *hit = &c.wins[wi];
                int lx, ly;
                if (hit->fullscreen)
                {
                    lx = c.mx - hit->x;
                    ly = c.my - hit->y;
                }
                else
                {
                    lx = c.mx - (hit->x + FRAME_X);
                    ly = c.my - (hit->y + FRAME_TOP);
                }
                comp_dispatch_motion(&c, hit, lx, ly, c.mx, c.my);
            }
        }

        /* Lifecycle scan: reap, promote starters, timeout hung ones,
           SIGKILL closes past deadline. */
        for (int i = 0; i < WM_MAX_WIN; i++)
        {
            struct comp_win *w = &c.wins[i];
            int st = 0;
            long r;
            if (!w->used || w->pid <= 0)
                continue;
            r = sys_waitpid_nb((int)w->pid, &st);
            if (r == w->pid)
            {
                int was_closing = w->closing;
                int chained = w->chain_login && st == 0;
                int login_ok = w->is_login && st == 0;
                w->pid = -1;
                w->closing = 0;
                w->chain_login = 0;
                if (w->evfd >= 0)
                {
                    close(w->evfd);
                    w->evfd = -1;
                }
                if (was_closing)
                {
                    comp_free_slot(&c, w);
                }
                else if (login_ok)
                {
                    long suid, sgid;
                    if (consume_session_file(w->shmid, &suid, &sgid) == 0 &&
                        win_reopen_session(&c, w, suid, sgid))
                    {
                        printf("guixd: session started (uid=%ld gid=%ld)\n",
                               suid, sgid);
                    }
                    else
                    {
                        printf("guixd: bad session handoff, reopening login\n");
                        w->dead = 0;
                        if (comp_open_client(&c, "Login", "axlogin", 0, -1,
                                             -1, 1, 1))
                        {
                            w->is_login = 1;
                            w->fullscreen = 1;
                            w->noclose = 1;
                        }
                        else
                        {
                            w->dead = 1;
                            w->status = -1;
                            comp_elect_focus(&c);
                        }
                    }
                }
                else if (st == 0)
                {
                    if (chained)
                        c.queued_login = 1;
                    comp_free_slot(&c, w);
                }
                else
                {
                    w->dead = 1;
                    w->status = st;
                    comp_elect_focus(&c);
                    if (chained)
                        c.queued_login = 1;
                }
                state_changed = 1;
                continue;
            }
            if (!w->live && !w->dead)
            {
                if (w->hdr && w->hdr->ready)
                {
                    w->live = 1;
                    state_changed = 1;
                }
                else if (now >= w->ready_deadline)
                {
                    printf("guixd: '%s' never became ready, closing\n",
                           w->title);
                    comp_request_close(&c, w, now);
                    state_changed = 1;
                }
            }
            else if (w->closing && now >= w->close_deadline)
            {
                kill((int)w->pid, SIGKILL);
            }
        }

        /* First-boot setup finished: chain into login. */
        if (c.queued_login)
        {
            c.queued_login = 0;
            comp_open_client(&c, "Login", "axlogin", 0, -1, -1, 1, 1);
            state_changed = 1;
        }

        /* Async fullscreen demo: re-grab input when reaped. */
        if (c.gfx_pid > 0)
        {
            int gst = 0;
            if (sys_waitpid_nb((int)c.gfx_pid, &gst) == c.gfx_pid)
            {
                c.gfx_pid = -1;
                axgui_grab(c.input_fd, 1);
                axgui_poll(c.input_fd, ev, 64);
                state_changed = 1;
            }
        }

        /* Client repaint detected via seqlock. */
        for (int i = 0; i < WM_MAX_WIN; i++)
        {
            struct comp_win *w = &c.wins[i];
            if (w->used && w->live && !w->dead && !w->closing && w->hdr &&
                w->hdr->ready && !(w->hdr->seq & 1u) &&
                w->hdr->seq != w->seq)
                state_changed = 1;
        }

        /* Desktop empty: reopen terminal after backoff. */
        {
            int any = comp_any_windows(&c);
            long wait;
            if (!any)
            {
                wait = 2L << (c.spawn_fails > 4 ? 4 : c.spawn_fails);
                if (wait > 30)
                    wait = 30;
                if (c.empty_since < 0)
                    c.empty_since = now;
                if (now - c.empty_since >= wait)
                {
                    if (comp_open_client(&c, "Terminal", "axterm", 0,
                                         c.sess_uid, c.sess_gid, 0, 0))
                    {
                        c.spawn_fails = 0;
                        c.empty_since = -1;
                        state_changed = 1;
                    }
                    else
                    {
                        c.spawn_fails++;
                        c.empty_since = now;
                    }
                }
            }
            else
            {
                c.empty_since = -1;
                c.spawn_fails = 0;
            }
        }

        /* Clock tick. */
        if (now != last_clock)
        {
            last_clock = now;
            state_changed = 1;
        }

        /* Render / present / cursor-only. */
        if (state_changed)
        {
            comp_render_frame(&c);
            axgui_present(&c.fb);
        }
        else if (mouse_moved)
        {
            comp_redraw_cursor_only(&c, &c.fb);
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
            target_us = axgui_auto_target_us(c.caps_ok ? &c.caps : 0,
                                             active, avg_us);
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
}
