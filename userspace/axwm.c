/* axwm — mini window manager / desktop environment for axiomeOS.
   Builds on the shared compositor core (userspace/compositor.h). Differs
   from guixd: no session management, no login chaining, no first-boot
   setup — just a simple desktop with draggable/resizable windows and an
   "Exit" button that returns to the console. */

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

static void open_terminal(struct comp_ctx *c, const char *cmd)
{
    comp_open_client(c, cmd && cmd[0] ? cmd : "Terminal", "axterm",
                     cmd, -1, -1, 0, 0);
}

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

int main(int argc, char **argv)
{
    struct comp_ctx c;
    struct axinput_event ev[64];
    int input_fd;
    int g_exit_req = 0;
    uint64_t avg_us = AXGUI_TARGET_60_US;
    (void)argc;
    (void)argv;

    memset(&c, 0, sizeof(c));
    c.fb.fd = -1;
    if (axgui_dri_open(&c.fb) < 0)
    {
        printf("axwm: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    input_fd = axgui_input_open();
    if (input_fd < 0)
    {
        printf("axwm: no input device (need input0)\n");
        axgui_close(&c.fb);
        return 1;
    }
    c.input_fd = input_fd;
    axgui_grab(input_fd, 1);
    axgui_poll(input_fd, ev, 64);

    comp_shm_pool_create(&c);
    scan_apps(&c);
    open_terminal(&c, 0);

    for (;;)
    {
        uint64_t frame_start_us = axgui_now_us();
        int n = axgui_poll(input_fd, ev, 64);
        int state_changed = 0;
        int mouse_moved = 0;
        int motion_pending = 0;
        long now = (long)time(0);

        for (int i = 0; i < n; i++)
        {
            if (ev[i].type == AXINPUT_TYPE_MOUSE)
            {
                int nx = c.mx + ev[i].dx;
                int ny = c.my + ev[i].dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx >= (int)c.fb.w) nx = (int)c.fb.w - 1;
                if (ny >= (int)c.fb.h) ny = (int)c.fb.h - 1;
                mouse_moved = (ev[i].dx || ev[i].dy);
                c.mx = nx;
                c.my = ny;
                c.btn = ev[i].code;
                int hit = comp_win_at(&c, c.mx, c.my);
                if (hit >= 0)
                {
                    struct comp_win *w = &c.wins[hit];
                    int lx = c.mx - (w->x + FRAME_X);
                    int ly = c.my - (w->y + FRAME_TOP);
                    if (w == comp_focused_win(&c) &&
                        lx >= 0 && ly >= 0 && lx < (int)w->w &&
                        ly < (int)w->h &&
                        (lx != w->last_mx || ly != w->last_my ||
                         c.btn != w->last_btn))
                    {
                        struct wm_event me;
                        me.type = WM_EV_MOUSE;
                        me.code = c.btn;
                        me.x = lx;
                        me.y = ly;
                        if (axgui_wm_send(w->evfd, &me) == 0)
                        {
                            w->last_mx = lx;
                            w->last_my = ly;
                            w->last_btn = c.btn;
                        }
                    }
                }
            }
            else if (ev[i].type == AXINPUT_TYPE_KEY)
            {
                uint32_t kc = ev[i].code;
                if (kc == '`')
                {
                    c.menu = !c.menu;
                }
                else
                {
                    struct comp_win *f = comp_focused_win(&c);
                    if (f && f->pid > 0 && !f->dead && f->evfd >= 0)
                    {
                        struct wm_event ke;
                        ke.type = WM_EV_KEY;
                        ke.code = kc;
                        ke.x = 0;
                        ke.y = 0;
                        if (axgui_wm_send(f->evfd, &ke) < 0)
                            comp_request_close(&c, f, now);
                    }
                    state_changed = 1;
                }
            }
        }

        /* Detect press edge: left was just pressed */
        static uint32_t prev_btn = 0;
        if ((c.btn & AXINPUT_BTN_LEFT) && !(prev_btn & AXINPUT_BTN_LEFT))
        {
            if (comp_handle_click(&c, c.mx, c.my, now))
                state_changed = 1;
        }
        if (!(c.btn & AXINPUT_BTN_LEFT) && (prev_btn & AXINPUT_BTN_LEFT))
        {
            if (c.drag_win >= 0 && c.drag_win < WM_MAX_WIN)
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
        }
        prev_btn = c.btn;

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

        /* Reap exited clients. */
        for (int i = 0; i < WM_MAX_WIN; i++)
        {
            struct comp_win *w = &c.wins[i];
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
                        comp_free_slot(&c, w);
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

        if (!comp_any_windows(&c))
            open_terminal(&c, 0);

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
            uint64_t elapsed_us = (now_us > frame_start_us) ? now_us - frame_start_us : 0;
            uint64_t target_us;
            long remain_us;
            int active = state_changed || mouse_moved;
            avg_us = (avg_us * 7 + elapsed_us) / 8;
            if (avg_us < 1000)
                avg_us = elapsed_us;
            target_us = axgui_auto_target_us(0, active, avg_us);
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

    /* Close all windows and return to console. */
    for (int i = 0; i < WM_MAX_WIN; i++)
        if (c.wins[i].used)
            comp_close_slot(&c, &c.wins[i]);
    axgui_grab(input_fd, 0);
    close(input_fd);
    axgui_close(&c.fb);
    printf("\n[axwm closed, back to console]\n");
    return 0;
}
