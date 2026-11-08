/* axterm — terminal emulator for axiomeOS (/Binaries/axterm).
   Connects to /devices/axw0, creates an axw surface + shm buffer via axmui,
   spawns /bin/sh, renders VT100 output directly into the surface buffer,
   and commits to the compositor.

   axterm is only a terminal emulator: it spawns /bin/sh with pipes on
   stdin/stdout/stderr, forwards keystrokes to that shell, parses the shell's
   output as a VT100-compatible byte stream, and renders the resulting screen.
   There is no command interpreter here; `exit`, `hello`, pipelines and shell
   builtins all belong to /bin/sh. */

#include "axw-protocol.h"
#include "axw-client.h"
#include "axgui.h"
#include "wm_abi.h"
#include "axmui/axmui.h"
#include "errno.h"
#include "signal.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 64
#define TERM_OUT_CHUNK 512
#define AXTERM_MAX_FD 32 /* must match kernel MAX_FD (kernel/vfs.h) */

#define AXTERM_W WM_WIN_W
#define AXTERM_H WM_WIN_H

/* Palette (canonical 0x00RRGGBB). */
#define C_BG      0x101418u
#define C_BAR     0x1E2836u
#define C_FG      0xD8DEE9u
#define C_ACCENT  0x5E81ACu

struct term_state {
    int cols;
    int rows;
    int cx;
    int cy;
    int saved_cx;
    int saved_cy;
    int esc; /* 0=text, 1=after ESC, 2=CSI, 3=OSC, 4=charset, 5=OSC ESC */
    int nparams;
    int params[8];
    char cells[TERM_MAX_ROWS][TERM_MAX_COLS];
};

struct shell_session {
    long pid;
    int to_shell;   /* write end: shell stdin */
    int from_shell; /* read end: shell stdout/stderr */
};

/* Global terminal state (accessed by axw event handler). */
static struct term_state g_term;

/* ---- terminal core ---- */

static void term_clamp_cursor(struct term_state *t)
{
    if (t->cx < 0)
        t->cx = 0;
    if (t->cy < 0)
        t->cy = 0;
    if (t->cx >= t->cols)
        t->cx = t->cols - 1;
    if (t->cy >= t->rows)
        t->cy = t->rows - 1;
}

static void term_clear(struct term_state *t)
{
    int r, c;
    for (r = 0; r < t->rows; r++)
        for (c = 0; c < t->cols; c++)
            t->cells[r][c] = ' ';
    t->cx = 0;
    t->cy = 0;
    t->saved_cx = 0;
    t->saved_cy = 0;
    t->esc = 0;
    t->nparams = 0;
}

static void term_init(struct term_state *t, int cols, int rows)
{
    int r, c;
    if (cols < 8)
        cols = 8;
    if (rows < 4)
        rows = 4;
    if (cols > TERM_MAX_COLS)
        cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS)
        rows = TERM_MAX_ROWS;
    t->cols = cols;
    t->rows = rows;
    for (r = 0; r < TERM_MAX_ROWS; r++)
        for (c = 0; c < TERM_MAX_COLS; c++)
            t->cells[r][c] = ' ';
    term_clear(t);
}

static void term_scroll(struct term_state *t)
{
    int r, c;
    if (t->rows <= 1)
        return;
    for (r = 0; r + 1 < t->rows; r++)
        for (c = 0; c < t->cols; c++)
            t->cells[r][c] = t->cells[r + 1][c];
    for (c = 0; c < t->cols; c++)
        t->cells[t->rows - 1][c] = ' ';
}

static void term_line_feed(struct term_state *t)
{
    t->cy++;
    if (t->cy >= t->rows)
    {
        term_scroll(t);
        t->cy = t->rows - 1;
    }
}

static void term_put(struct term_state *t, char ch)
{
    if (t->cy < 0 || t->cy >= t->rows)
        term_clamp_cursor(t);
    if (t->cx < 0 || t->cx >= t->cols)
        term_clamp_cursor(t);
    t->cells[t->cy][t->cx] = ch;
    t->cx++;
    if (t->cx >= t->cols)
    {
        t->cx = 0;
        term_line_feed(t);
    }
}

static int term_param(struct term_state *t, int idx, int def)
{
    if (idx < 0 || idx >= t->nparams)
        return def;
    if (t->params[idx] == 0)
        return def;
    return t->params[idx];
}

static void term_erase_line(struct term_state *t, int mode)
{
    int c;
    term_clamp_cursor(t);
    if (mode == 2)
    {
        for (c = 0; c < t->cols; c++)
            t->cells[t->cy][c] = ' ';
    }
    else if (mode == 1)
    {
        for (c = 0; c <= t->cx; c++)
            t->cells[t->cy][c] = ' ';
    }
    else
    {
        for (c = t->cx; c < t->cols; c++)
            t->cells[t->cy][c] = ' ';
    }
}

static void term_erase_display(struct term_state *t, int mode)
{
    int r, c;
    if (mode == 2 || mode == 3)
    {
        term_clear(t);
        return;
    }
    term_clamp_cursor(t);
    if (mode == 1)
    {
        for (r = 0; r < t->cy; r++)
            for (c = 0; c < t->cols; c++)
                t->cells[r][c] = ' ';
        for (c = 0; c <= t->cx; c++)
            t->cells[t->cy][c] = ' ';
    }
    else
    {
        for (c = t->cx; c < t->cols; c++)
            t->cells[t->cy][c] = ' ';
        for (r = t->cy + 1; r < t->rows; r++)
            for (c = 0; c < t->cols; c++)
                t->cells[r][c] = ' ';
    }
}

static void term_handle_csi(struct term_state *t, char final)
{
    int n, row, col, i;
    switch (final)
    {
    case 'A':
        n = term_param(t, 0, 1);
        t->cy -= n;
        term_clamp_cursor(t);
        break;
    case 'B':
    case 'e':
        n = term_param(t, 0, 1);
        t->cy += n;
        term_clamp_cursor(t);
        break;
    case 'C':
    case 'a':
        n = term_param(t, 0, 1);
        t->cx += n;
        term_clamp_cursor(t);
        break;
    case 'D':
        n = term_param(t, 0, 1);
        t->cx -= n;
        term_clamp_cursor(t);
        break;
    case 'E':
        n = term_param(t, 0, 1);
        t->cx = 0;
        t->cy += n;
        term_clamp_cursor(t);
        break;
    case 'F':
        n = term_param(t, 0, 1);
        t->cx = 0;
        t->cy -= n;
        term_clamp_cursor(t);
        break;
    case 'G':
    case '`':
        col = term_param(t, 0, 1);
        t->cx = col - 1;
        term_clamp_cursor(t);
        break;
    case 'd':
        row = term_param(t, 0, 1);
        t->cy = row - 1;
        term_clamp_cursor(t);
        break;
    case 'H':
    case 'f':
        row = term_param(t, 0, 1);
        col = term_param(t, 1, 1);
        t->cy = row - 1;
        t->cx = col - 1;
        term_clamp_cursor(t);
        break;
    case 'J':
        n = t->nparams > 0 ? t->params[0] : 0;
        term_erase_display(t, n);
        break;
    case 'K':
        n = t->nparams > 0 ? t->params[0] : 0;
        term_erase_line(t, n);
        break;
    case 'm':
        break;
    case 's':
        t->saved_cx = t->cx;
        t->saved_cy = t->cy;
        break;
    case 'u':
        t->cx = t->saved_cx;
        t->cy = t->saved_cy;
        term_clamp_cursor(t);
        break;
    case 'S':
        n = term_param(t, 0, 1);
        for (i = 0; i < n; i++)
            term_scroll(t);
        break;
    case 'T':
        n = term_param(t, 0, 1);
        for (i = 0; i < n && t->rows > 1; i++)
        {
            int r, c;
            for (r = t->rows - 1; r > 0; r--)
                for (c = 0; c < t->cols; c++)
                    t->cells[r][c] = t->cells[r - 1][c];
            for (c = 0; c < t->cols; c++)
                t->cells[0][c] = ' ';
        }
        break;
    default:
        break;
    }
}

static void term_feed(struct term_state *t, const char *buf, size_t len)
{
    size_t i;
    if (!t || !buf)
        return;
    for (i = 0; i < len; i++)
    {
        unsigned char b = (unsigned char)buf[i];
        if (t->esc == 0)
        {
            if (b == 27)
            {
                t->esc = 1;
            }
            else if (b == '\a' || b == 0)
            {
            }
            else if (b == '\b')
            {
                if (t->cx > 0)
                    t->cx--;
            }
            else if (b == '\t')
            {
                t->cx = (t->cx + 8) & ~7;
                if (t->cx >= t->cols)
                {
                    t->cx = 0;
                    term_line_feed(t);
                }
            }
            else if (b == '\n')
            {
                t->cx = 0;
                term_line_feed(t);
            }
            else if (b == '\r')
            {
                t->cx = 0;
            }
            else if (b == '\v' || b == '\f')
            {
                term_line_feed(t);
            }
            else if (b < 32 || b == 127)
            {
            }
            else
            {
                term_put(t, (char)b);
            }
        }
        else if (t->esc == 1)
        {
            if (b == '[')
            {
                t->esc = 2;
                t->nparams = 0;
                t->params[0] = 0;
            }
            else if (b == ']')
            {
                t->esc = 3;
            }
            else if (b == '(' || b == ')')
            {
                t->esc = 4;
            }
            else if (b == '7')
            {
                t->saved_cx = t->cx;
                t->saved_cy = t->cy;
                t->esc = 0;
            }
            else if (b == '8')
            {
                t->cx = t->saved_cx;
                t->cy = t->saved_cy;
                term_clamp_cursor(t);
                t->esc = 0;
            }
            else if (b == 'M')
            {
                if (t->cy > 0)
                {
                    t->cy--;
                }
                else if (t->rows > 1)
                {
                    int r, c;
                    for (r = t->rows - 1; r > 0; r--)
                        for (c = 0; c < t->cols; c++)
                            t->cells[r][c] = t->cells[r - 1][c];
                    for (c = 0; c < t->cols; c++)
                        t->cells[0][c] = ' ';
                }
                t->esc = 0;
            }
            else if (b == 'c')
            {
                term_clear(t);
                t->esc = 0;
            }
            else
            {
                t->esc = 0;
            }
        }
        else if (t->esc == 2)
        {
            if (b >= '0' && b <= '9')
            {
                if (t->nparams == 0)
                {
                    t->nparams = 1;
                    t->params[0] = 0;
                }
                t->params[t->nparams - 1] =
                    t->params[t->nparams - 1] * 10 + (int)(b - '0');
                if (t->params[t->nparams - 1] > 9999)
                    t->params[t->nparams - 1] = 9999;
            }
            else if (b == ';')
            {
                if (t->nparams == 0)
                {
                    t->nparams = 1;
                    t->params[0] = 0;
                }
                if (t->nparams < 8)
                {
                    t->params[t->nparams] = 0;
                    t->nparams++;
                }
            }
            else if (b == '?' || b == ' ' || b == '"' || b == '\'' ||
                     b == '$')
            {
            }
            else if (b >= '@' && b <= '~')
            {
                term_handle_csi(t, (char)b);
                t->esc = 0;
            }
            else
            {
                t->esc = 0;
            }
        }
        else if (t->esc == 3)
        {
            if (b == '\a')
                t->esc = 0;
            else if (b == 27)
                t->esc = 5;
        }
        else if (t->esc == 4)
        {
            t->esc = 0;
        }
        else if (t->esc == 5)
        {
            if (b == '\\')
                t->esc = 0;
            else if (b == 27)
                t->esc = 5;
            else
                t->esc = 3;
        }
        else
        {
            t->esc = 0;
        }
    }
}

static void term_status(struct term_state *t, const char *s)
{
    if (!s)
        return;
    term_feed(t, s, strlen(s));
    term_feed(t, "\n", 1);
}

static void term_draw(struct axgui_fb *fb, struct term_state *t, int x0,
                      int y0)
{
    int r;
    if (!fb || !fb->px || !t)
        return;
    for (r = 0; r < t->rows; r++)
    {
        axgui_text_cell(fb, t->cells[r], t->cols, x0,
                        y0 + r * FONT_HEIGHT, C_FG, C_BG);
    }
    if (t->cx >= 0 && t->cx < t->cols && t->cy >= 0 && t->cy < t->rows)
    {
        axgui_fill(fb, x0 + t->cx * FONT_WIDTH, y0 + t->cy * FONT_HEIGHT,
                   FONT_WIDTH, FONT_HEIGHT, C_ACCENT);
    }
}

/* Translate one compositor key code into terminal input bytes. */
static int term_key_bytes(uint32_t code, char *out)
{
    if (!out)
        return 0;
    if (code == '\n' || code == '\r')
    {
        out[0] = '\n';
        return 1;
    }
    if (code == 127 || code == 8)
    {
        out[0] = 127;
        return 1;
    }
    if (code == 27)
    {
        out[0] = 27;
        return 1;
    }
    if (code == AXINPUT_KEY_UP)
    {
        out[0] = 27; out[1] = '['; out[2] = 'A';
        return 3;
    }
    if (code == AXINPUT_KEY_DOWN)
    {
        out[0] = 27; out[1] = '['; out[2] = 'B';
        return 3;
    }
    if (code == AXINPUT_KEY_RIGHT)
    {
        out[0] = 27; out[1] = '['; out[2] = 'C';
        return 3;
    }
    if (code == AXINPUT_KEY_LEFT)
    {
        out[0] = 27; out[1] = '['; out[2] = 'D';
        return 3;
    }
    if (code == AXINPUT_KEY_HOME)
    {
        out[0] = 27; out[1] = '['; out[2] = 'H';
        return 3;
    }
    if (code == AXINPUT_KEY_END)
    {
        out[0] = 27; out[1] = '['; out[2] = 'F';
        return 3;
    }
    if (code >= 32 && code < 127)
    {
        out[0] = (char)code;
        return 1;
    }
    if (code >= 1 && code < 32)
    {
        out[0] = (char)code;
        return 1;
    }
    return 0;
}

/* ---- helper utilities ---- */

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    int tries = 0;
    if (!buf)
        return -1;
    while (off < len)
    {
        long r = write(fd, buf + off, len - off);
        if (r > 0)
        {
            off += (size_t)r;
            tries = 0;
            continue;
        }
        if (r == 0)
        {
            if (++tries > 1000)
                return -1;
            axgui_msleep(1);
            continue;
        }
        return -1;
    }
    return 0;
}

static void close_extra_fds(int keep_a, int keep_b, int keep_c, int keep_d)
{
    int fd;
    for (fd = 0; fd < AXTERM_MAX_FD; fd++)
    {
        if (fd == 0 || fd == 1 || fd == 2)
            continue;
        if (fd == keep_a || fd == keep_b || fd == keep_c || fd == keep_d)
            continue;
        close(fd);
    }
}

/* SIGKILL the process tree rooted at `root` (children before parents). */
static void kill_process_tree(long root)
{
    struct proc_info ps[64];
    int marked[64];
    int depth[64];
    long self;
    int n, i, j, pass;
    if (root <= 0)
        return;
    self = sys_getpid();
    if (root == self)
        return;
    for (pass = 0; pass < 3; pass++)
    {
        int maxd = 0;
        int changed;
        int root_seen = 0;
        n = sys_ps(ps, 64);
        if (n <= 0)
        {
            kill((int)root, SIGKILL);
            return;
        }
        if (n > 64)
            n = 64;
        for (i = 0; i < n; i++)
        {
            marked[i] = 0;
            depth[i] = -1;
            if (ps[i].pid == (int)root)
                root_seen = 1;
        }
        if (!root_seen)
            return;
        for (i = 0; i < n; i++)
        {
            if (ps[i].pid == (int)root)
                marked[i] = 1;
        }
        for (;;)
        {
            changed = 0;
            for (i = 0; i < n; i++)
            {
                if (marked[i] || ps[i].pid <= 0)
                    continue;
                for (j = 0; j < n; j++)
                {
                    if (marked[j] && ps[i].parent_pid == ps[j].pid)
                    {
                        marked[i] = 1;
                        changed = 1;
                        break;
                    }
                }
            }
            if (!changed)
                break;
        }
        for (i = 0; i < n; i++)
        {
            if (ps[i].pid == (int)root)
                depth[i] = 0;
        }
        for (;;)
        {
            changed = 0;
            for (i = 0; i < n; i++)
            {
                if (!marked[i] || depth[i] >= 0)
                    continue;
                for (j = 0; j < n; j++)
                {
                    if (marked[j] && depth[j] >= 0 &&
                        ps[i].parent_pid == ps[j].pid)
                    {
                        depth[i] = depth[j] + 1;
                        changed = 1;
                        break;
                    }
                }
            }
            if (!changed)
                break;
        }
        for (i = 0; i < n; i++)
        {
            if (!marked[i])
                continue;
            if (depth[i] < 0)
                depth[i] = 1000;
            if (depth[i] > maxd && depth[i] < 1000)
                maxd = depth[i];
        }
        {
            int d;
            for (d = maxd + 1; d >= 0; d--)
            {
                for (i = 0; i < n; i++)
                {
                    if (marked[i] && depth[i] == d &&
                        ps[i].pid != (int)self && ps[i].pid != (int)root)
                        kill(ps[i].pid, SIGKILL);
                }
            }
        }
        kill((int)root, SIGKILL);
    }
}

/* Blocking wait with a deadline. */
static int reap_child_deadline(long pid, int *status, int timeout_ms)
{
    int waited = 0;
    int st = 0;
    long q;
    if (pid <= 0)
        return -1;
    for (;;)
    {
        errno = 0;
        q = sys_waitpid_nb((int)pid, &st);
        if (q == pid)
        {
            if (status)
                *status = st;
            return 0;
        }
        if (q < 0 && errno == ECHILD)
            return 0;
        if (waited >= timeout_ms)
            return -1;
        axgui_msleep(10);
        waited += 10;
    }
}

static int shell_spawn(struct shell_session *s, int uid, int gid)
{
    int to_shell[2] = { -1, -1 };
    int from_shell[2] = { -1, -1 };
    long pid;
    if (!s)
        return -1;
    s->pid = -1;
    s->to_shell = -1;
    s->from_shell = -1;
    if (pipe(to_shell) < 0 || pipe(from_shell) < 0)
    {
        if (to_shell[0] >= 0 || to_shell[1] >= 0)
        {
            close(to_shell[0]);
            close(to_shell[1]);
        }
        return -1;
    }
    pid = sys_fork();
    if (pid < 0)
    {
        close(to_shell[0]);
        close(to_shell[1]);
        close(from_shell[0]);
        close(from_shell[1]);
        return -1;
    }
    if (pid == 0)
    {
        char *argv[2];
        char *envp[1];
        int fd;
        if (dup2(to_shell[0], 0) < 0)
            sys_exit(127);
        if (dup2(from_shell[1], 1) < 0)
            sys_exit(127);
        if (dup2(from_shell[1], 2) < 0)
            sys_exit(127);
        for (fd = 3; fd < AXTERM_MAX_FD; fd++)
            close(fd);
        if (uid >= 0)
        {
            if (setgid((gid_t)gid) < 0 || setuid((uid_t)uid) < 0)
            {
                write(2, "axterm: cannot drop privileges\n", 31);
                sys_exit(127);
            }
        }
        argv[0] = "sh";
        argv[1] = 0;
        envp[0] = 0;
        execve("/bin/sh", argv, envp);
        write(2, "axterm: cannot exec /bin/sh\n", 28);
        sys_exit(127);
    }
    close(to_shell[0]);
    close(from_shell[1]);
    s->pid = pid;
    s->to_shell = to_shell[1];
    s->from_shell = from_shell[0];
    return 0;
}

/* ---- axw compositor client mode using axmui for surface lifecycle ---- */

static int g_axw_fd = -1;
static struct axw_display *g_disp = 0;
static struct axw_compositor *g_comp = 0;
static struct axw_shm *g_shm = 0;
static struct axw_surface *g_surf = 0;
static struct axw_buffer *g_buf = 0;
static long g_shmid = -1;
static uint32_t g_surf_id = 0;
static uint8_t *g_buf_map = 0;
static struct axgui_fb g_fb;

static int g_running = 1;
static int g_shell_in = -1;
static long g_shell_pid = -1;
static int g_shell_gone = 0;
static int g_shell_status = 1;
static int g_out_eof = 0;
static int g_need_render = 1;

static int axterm_axw_event_handler(struct axw_display *d,
                                    uint32_t sender_id,
                                    uint16_t opcode,
                                    const uint32_t *args, int nargs)
{
    (void)d;
    /* Only process events addressed to our own surface. */
    if (!g_surf || sender_id != g_surf_id)
        return 0;

    if (opcode == AXW_KEYBOARD_KEY)
    {
        if (nargs < 2)
            return 0;
        uint32_t kc = args[1];
        char seq[4];
        int n = term_key_bytes(kc, seq);
        if (n > 0 && g_shell_in >= 0)
            write_all(g_shell_in, seq, (size_t)n);
        return 0;
    }
    if (opcode == AXW_POINTER_BUTTON || opcode == AXW_POINTER_MOTION)
    {
        /* Terminal doesn't use mouse input. */
        return 0;
    }
    return 0;
}

static void axterm_render_frame(void)
{
    axgui_fill(&g_fb, 0, 0, (int)g_fb.w, (int)g_fb.h, C_BG);
    axgui_fill(&g_fb, 0, 0, (int)g_fb.w, 24, C_BAR);
    axgui_text(&g_fb, "axterm - /bin/sh", 8, 4, C_FG, C_BAR);
    term_draw(&g_fb, &g_term, 8, 28);
    axw_surface_damage(g_surf, 0, 0, g_fb.w, g_fb.h);
    axw_surface_commit(g_surf);
}

static int run_axw_client(const char *initial_cmd, int uid, int gid)
{
    struct shell_session sh;
    char out[TERM_OUT_CHUNK];
    int shell_status = 1;

    /* Connect to display server */
    g_axw_fd = axw_connect("/devices/axw0");
    if (g_axw_fd < 0)
        g_axw_fd = axw_connect("/Devices/axw0");
    if (g_axw_fd < 0)
    {
        printf("axterm: cannot connect to /devices/axw0\n");
        return 1;
    }

    g_disp = axw_display_create(g_axw_fd);
    if (!g_disp)
    {
        printf("axterm: display create failed\n");
        close(g_axw_fd);
        return 1;
    }
    axw_display_set_event_handler(g_disp, axterm_axw_event_handler, NULL);

    /* Bind globals */
    struct axw_registry *reg = axw_display_get_registry(g_disp);
    if (!reg)
    {
        printf("axterm: registry bind failed\n");
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }
    g_comp = axw_registry_bind_compositor(reg);
    g_shm = axw_registry_bind_shm(reg);
    if (!g_comp || !g_shm)
    {
        printf("axterm: compositor/shm bind failed\n");
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }

    /* Use axmui only for surface lifecycle (shm + buffer + surface + attach) */
    axmui_app_t *app = axmui_app_create();
    if (!app)
    {
        printf("axterm: axmui_app_create failed\n");
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }

    axmui_window_t *win = axmui_window_create(app, "Terminal", AXTERM_W, AXTERM_H);
    if (!win)
    {
        printf("axterm: axmui_window_create failed\n");
        axmui_app_destroy(app);
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }

    if (axmui_window_attach_axw(win, g_disp, g_comp, g_shm, AXTERM_W, AXTERM_H) < 0)
    {
        printf("axterm: axmui_window_attach_axw failed\n");
        axmui_app_destroy(app);
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }
    g_surf_id = win->surf_id;
    g_surf = win->surf;
    g_fb = win->fb; /* use axmui's framebuffer for direct drawing */

    /* Destroy axmui app - we only needed it for surface lifecycle */
    axmui_app_destroy(app);

    /* Spawn shell */
    if (shell_spawn(&sh, uid, gid) < 0)
    {
        printf("axterm: cannot start /bin/sh\n");
        shm_destroy(g_shmid);
        axw_display_destroy(g_disp);
        close(g_axw_fd);
        return 1;
    }

    if (initial_cmd && initial_cmd[0])
    {
        size_t n = strlen(initial_cmd);
        if (n + 1 < sizeof(out))
        {
            out[n++] = '\n';
            out[n] = 0;
            write_all(sh.to_shell, out, n);
        }
    }

    /* Keep to_shell (write end) open as g_shell_in for keyboard input. */
    g_shell_in = sh.to_shell;
    g_shell_pid = sh.pid;

    /* Make shell output pipe non-blocking */
    fcntl(sh.from_shell, F_SETFL, O_NONBLOCK);

    /* Initial render */
    axterm_render_frame();

    for (;;)
    {
        /* Dispatch axw events (keyboard input, etc.) */
        int n = axw_dispatch(g_disp);
        (void)n;

        if (!g_running)
            break;

        /* Drain shell output */
        long r = read(sh.from_shell, out, sizeof(out));
        if (r > 0)
        {
            term_feed(&g_term, out, (size_t)r);
            g_need_render = 1;
        }
        else if (r == 0)
        {
            g_out_eof = 1;
        }

        /* Check if shell exited */
        if (!g_shell_gone)
        {
            int st = 0;
            long q;
            errno = 0;
            q = sys_waitpid_nb((int)sh.pid, &st);
            if (q == sh.pid)
            {
                g_shell_gone = 1;
                g_shell_status = st;
                char done[64];
                snprintf(done, sizeof(done), "[shell exited, status %d]", st);
                term_status(&g_term, done);
                g_need_render = 1;
            }
            else if (q < 0 && errno == ECHILD)
            {
                g_shell_gone = 1;
            }
        }

        /* Re-render if dirty */
        if (g_need_render)
        {
            g_need_render = 0;
            axterm_render_frame();
        }

        if (g_out_eof && g_shell_gone)
            break;
        if (g_out_eof && !g_shell_gone)
        {
            kill_process_tree(sh.pid);
            if (reap_child_deadline(sh.pid, &g_shell_status, 2000) == 0)
            {
                g_shell_gone = 1;
                break;
            }
            axgui_msleep(10);
        }
        sys_yield();
    }

    /* Cleanup */
    kill_process_tree(sh.pid);
    if (g_shell_in >= 0)
        close(g_shell_in);
    close(sh.from_shell);
    shm_destroy(g_shmid);
    axw_display_destroy(g_disp);
    close(g_axw_fd);
    g_surf = 0;
    g_disp = 0;
    g_comp = 0;
    g_shm = 0;
    g_buf = 0;

    return g_shell_status;
}

int main(int argc, char **argv)
{
    const char *initial_cmd = 0;
    int uid = -1;
    int gid = -1;

    /* Parse args: axterm [-c cmd...] [--user <uid> <gid>] */
    int i = 1;
    while (i < argc)
    {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            initial_cmd = argv[i + 1];
            i += 2;
        }
        else if (strcmp(argv[i], "--user") == 0 && i + 2 < argc)
        {
            long u, g;
            char *end;
            u = strtol(argv[i + 1], &end, 10);
            g = strtol(argv[i + 2], &end, 10);
            if (*end == 0 && u >= 0 && g >= 0)
            {
                uid = (int)u;
                gid = (int)g;
            }
            i += 3;
        }
        else
        {
            i++;
        }
    }

    (void)initial_cmd;

    return run_axw_client(initial_cmd, uid, gid);
}
