/* axterm — terminal emulator for axiomeOS (/Binaries/axterm).
   Two modes:
   * standalone (no args): fullscreen /Devices/dri0 terminal, holds the GUI
     input grab while a real /bin/sh session runs underneath.
   * compositor client (`axterm --wm <shmid> <w> <h> <evfd> [--user <uid> <gid>]
   *   [-c cmd...]`):
     draws into the window SHM segment and reads wm_events from <evfd>.

   axterm is only a terminal emulator: it spawns /bin/sh with pipes on
   stdin/stdout/stderr, forwards keystrokes to that shell, parses the shell's
   output as a VT100-compatible byte stream, and renders the resulting screen.
   There is no command interpreter here; `exit`, `hello`, pipelines and shell
   builtins all belong to /bin/sh. */

#include "axclient.h"
#include "axmui/axmui.h"
#include "errno.h"
#include "signal.h"

#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 64
#define TERM_CMD_MAX 512
#define TERM_OUT_CHUNK 512
#define AXTERM_MAX_FD 32 /* must match kernel MAX_FD in kernel/vfs.h */

/* Palette (canonical 0x00RRGGBB). */
#define C_BG      0x101418u
#define C_BAR     0x1E2836u
#define C_FG      0xD8DEE9u
#define C_PROMPT  0x88C0D0u
#define C_ACCENT  0x5E81ACu
#define C_DIM     0x4C566Au

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
    int to_shell;   /* parent/helper write end: shell stdin */
    int from_shell; /* parent read end: shell stdout/stderr */
};

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
        /* SGR attributes (colours/bold) are accepted and ignored: this
           emulator keeps the fixed terminal palette. */
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
                /* bell and NUL: no visible effect */
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
                /* Match the kernel console: newline means CR+LF. */
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
                /* Other controls (including DEL) are not printable. */
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
                /* Unknown single-character escape: drop it. */
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
                /* Private/intermediate bytes: accepted, no behaviour. */
            }
            else if (b >= '@' && b <= '~')
            {
                term_handle_csi(t, (char)b);
                t->esc = 0;
            }
            else
            {
                /* Malformed sequence: drop it rather than wedging. */
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

/* Translate one compositor key code into terminal input bytes. Keys the
   shell cannot use (menu/scroll function keys) produce no bytes instead of
   injecting garbage into the shell's stdin. */
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

/* Join argv[0..n) with spaces (for the window client's -c command). */
static void join_args(char **argv, int n, char *out, size_t cap)
{
    size_t pos = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < n; i++)
    {
        const char *a = argv[i];
        if (i > 0 && pos + 1 < cap)
            out[pos++] = ' ';
        while (*a && pos + 1 < cap)
            out[pos++] = *a++;
    }
    out[pos] = 0;
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

/* SIGKILL the process tree rooted at `root` (children before parents).
   Used only for terminal teardown: a closed window must not leave its shell
   or that shell's jobs behind. Never targets the caller itself.

   Two hard rules make this deterministic instead of "rng":
   * the root must be present in the CURRENT sys_ps snapshot, otherwise we
     kill nothing: a bare kill-by-pid after the root already exited could
     hit an unrelated recycled pid.
   * kills go children-first (depth order, root last) so no child is
     orphaned mid-pass; up to 3 snapshot passes catch late forks. */
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
            /* Table unreadable: fall back to the root alone rather than
               leaving the whole tree behind. */
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
                depth[i] = 1000; /* detached chain: kill first */
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
        /* Next pass re-snapshots and stops as soon as the root is gone. */
    }
}

/* Blocking wait with a deadline, so teardown never hangs forever if a child
   is already gone or refuses to die promptly. Returns 0 when reaped. */
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
            return 0; /* already gone (or never ours): counts as reaped */
        if (waited >= timeout_ms)
            return -1;
        axgui_msleep(10);
        waited += 10;
    }
}

static int shell_spawn(struct shell_session *s, int uid, int gid, int evfd)
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
        /* The shell must not retain the compositor event pipe (or any
           other inherited descriptor): a leaked read alias would defeat
           the window's EOF delivery, a leaked write alias the shell's. */
        if (evfd > 2)
            close(evfd);
        for (fd = 3; fd < AXTERM_MAX_FD; fd++)
            close(fd);
        /* Drop to the session user (axlogin passes --user after a successful
           login). Refuse to continue if the drop fails: never run a user
           session with the wrong credentials. */
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
    /* Parent keeps the terminal side of both pipes. */
    close(to_shell[0]);
    close(from_shell[1]);
    s->pid = pid;
    s->to_shell = to_shell[1];
    s->from_shell = from_shell[0];
    return 0;
}

/* ---- compositor client mode (`axterm --wm ...`) — axmui edition ----
 * Still spawns /bin/sh with the same pipe discipline (helper for input,
 * supervisor for rendering) but the supervisor now renders through axmui:
 * HTML describes the chrome (header + card + terminal container), CSS styles
 * it, and the terminal grid itself is a custom-draw view inside that layout.
 * No WebKit — every pixel is native software raster.
 */
static struct term_state *g_term_for_draw;

static void axmui_term_draw(struct axgui_fb *fb, axmui_view_t *view, void *ud){
    struct term_state *t=(struct term_state*)ud;
    if(!t||!fb||!view) return;
    int x0=view->abs_x + view->style.border_w + view->style.padding[3];
    int y0=view->abs_y + view->style.border_w + view->style.padding[0];
    term_draw(fb,t,x0,y0);
}

/* Keyboard pump: the only reader of the compositor event pipe. Forwards
   keystrokes to the shell, then tears the shell session down when the
   compositor closes the window (evfd EOF). */
static void window_keyboard_pump(int evfd, int shell_in, long shell_pid)
{
    struct wm_event ev;
    for (;;)
    {
        char seq[4];
        int n;
        if (axgui_wm_recv(evfd, &ev) < 0)
            break;
        if (ev.type != WM_EV_KEY)
            continue;
        n = term_key_bytes(ev.code, seq);
        if (n > 0 && write_all(shell_in, seq, (size_t)n) < 0)
            break;
    }
    kill_process_tree(shell_pid);
    close(shell_in);
    close(evfd);
}

static int run_wm_client(struct axclient *cx, char **cargv, int cargc,
                         int uid, int gid)
{
    struct shell_session sh;
    struct term_state term;
    char out[TERM_OUT_CHUNK];
    char cmd[TERM_CMD_MAX];
    long helper = -1;
    int evfd = cx->evfd;
    int shell_status = 1;
    int shell_gone = 0;
    int helper_gone = 0;
    int out_eof = 0;
    int closing = 0;

    if (shell_spawn(&sh, uid, gid, evfd) < 0)
    {
        printf("axterm-wm: cannot start /bin/sh\n");
        return 1;
    }
    if (cargv && cargc > 0)
    {
        join_args(cargv, cargc, cmd, sizeof(cmd));
        if (cmd[0])
        {
            size_t n = strlen(cmd);
            if (n + 1 < sizeof(cmd))
            {
                cmd[n++] = '\n';
                cmd[n] = 0;
                write_all(sh.to_shell, cmd, n);
            }
        }
    }
    helper = sys_fork();
    if (helper < 0)
    {
        kill_process_tree(sh.pid);
        reap_child_deadline(sh.pid, 0, 2000);
        close(sh.to_shell);
        close(sh.from_shell);
        return 1;
    }
    if (helper == 0)
    {
        close(sh.from_shell);
        window_keyboard_pump(evfd, sh.to_shell, sh.pid);
        sys_exit(0);
    }
    close(sh.to_shell);
    close(evfd);

    /* axmui chrome around the terminal */
    axmui_app_t *app=axmui_app_create();
    axmui_window_t *win=axmui_window_create(app,"Terminal", WM_WIN_W, WM_WIN_H);
    if(!win){ kill_process_tree(sh.pid); return 1; }
    /* attach to the already-validated window SHM */
    axmui_window_attach(win, cx);

    int cols = (WM_WIN_W - 16) / FONT_WIDTH;
    int rows = (WM_WIN_H - 16) / FONT_HEIGHT;
    if(cols<8) cols=8; if(rows<4) rows=4;
    term_init(&term, cols, rows);

    const char *html =
        "<div id='term' style='flex:1; background:#101418; padding:8px'></div>";
    const char *css = ".window{ padding:0; gap:0; background:#101418; }";
    axmui_window_set_html(win, html, css);
    axmui_view_t *termView=axmui_view_find(axmui_window_root(win),"term");
    if(termView){
        axmui_view_set_draw(termView, axmui_term_draw, &term);
        /* ensure term view expands */
        termView->style.flex_grow=1;
    }

    /* first frame */
    g_term_for_draw=&term;
    axclient_begin(cx);
    axmui_window_render(win);
    cx->hdr->ready = 1;
    axclient_commit(cx);

    for (;;)
    {
        long n = read(sh.from_shell, out, sizeof(out));
        if (n > 0)
        {
            term_feed(&term, out, (size_t)n);
            axclient_begin(cx);
            axmui_window_render(win);
            axclient_commit(cx);
        }
        else if (n == 0)
        {
            out_eof = 1;
        }
        else
        {
            out_eof = 1;
            closing = 1;
        }
        if (axclient_closed(cx) || axclient_stale(cx))
            closing = 1;
        if (helper > 0 && !helper_gone)
        {
            long hq;
            errno = 0;
            hq = sys_waitpid_nb((int)helper, 0);
            if (hq == helper || (hq < 0 && errno == ECHILD))
            {
                helper_gone = 1;
                closing = 1;
            }
        }
        if (!shell_gone)
        {
            int st = 0;
            long q;
            errno = 0;
            q = sys_waitpid_nb((int)sh.pid, &st);
            if (q == sh.pid)
            {
                char done[64];
                shell_gone = 1;
                shell_status = st;
                snprintf(done, sizeof(done), "[shell exited, status %d]", st);
                term_status(&term, done);
                axclient_begin(cx);
                axmui_window_render(win);
                axclient_commit(cx);
            }
            else if (q < 0 && errno == ECHILD)
            {
                shell_gone = 1;
            }
        }
        if (closing && !shell_gone)
            kill_process_tree(sh.pid);
        if (out_eof && shell_gone)
            break;
        if (out_eof && !shell_gone)
        {
            kill_process_tree(sh.pid);
            if (reap_child_deadline(sh.pid, &shell_status, 2000) == 0)
            {
                char done[64];
                shell_gone = 1;
                snprintf(done, sizeof(done), "[shell exited, status %d]", shell_status);
                term_status(&term, done);
                axclient_begin(cx);
                axmui_window_render(win);
                axclient_commit(cx);
                break;
            }
            axgui_msleep(10);
        }
    }

    axclient_begin(cx);
    axmui_window_render(win);
    axclient_commit(cx);
    close(sh.from_shell);
    if (helper > 0)
    {
        kill((int)helper, SIGKILL);
        sys_waitpid((int)helper, 0);
    }
    if(app) axmui_app_destroy(app);
    return shell_status;
}

/* ---- standalone mode (fullscreen DRI terminal) ---- */

/* Input pump for standalone mode: /Devices/input0 is already non-blocking,
   so one helper can poll it while the supervisor blocks on shell output. */
static void standalone_input_pump(int input_fd, int shell_in, long shell_pid)
{
    struct axinput_event ev[64];
    (void)shell_pid;
    for (;;)
    {
        int n = axgui_poll(input_fd, ev, 64);
        int i;
        for (i = 0; i < n; i++)
        {
            char seq[4];
            int m;
            if (ev[i].type != AXINPUT_TYPE_KEY)
                continue;
            m = term_key_bytes(ev[i].code, seq);
            if (m > 0)
                write_all(shell_in, seq, (size_t)m);
        }
        axgui_msleep(5);
    }
}

static int run_standalone(void)
{
    struct axgui_fb fb;
    struct shell_session sh;
    struct term_state term;
    char out[TERM_OUT_CHUNK];
    int input_fd = -1;
    long helper = -1;
    int shell_status = 1;
    int shell_gone = 0;
    int helper_gone = 0;
    int out_eof = 0;

    fb.fd = -1;
    if (axgui_dri_open(&fb) < 0)
    {
        printf("axterm: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    input_fd = axgui_input_open();
    if (input_fd >= 0)
        axgui_grab(input_fd, 1);
    close_extra_fds(input_fd, fb.fd, -1, -1);
    if (shell_spawn(&sh, -1, -1, -1) < 0)
    {
        printf("axterm: cannot start /bin/sh\n");
        goto stand_fail;
    }
    helper = sys_fork();
    if (helper < 0)
    {
        kill_process_tree(sh.pid);
        reap_child_deadline(sh.pid, 0, 2000);
        close(sh.to_shell);
        close(sh.from_shell);
        goto stand_fail;
    }
    if (helper == 0)
    {
        close(sh.from_shell);
        /* Keep only the console stdio, the polled input device and the
           shell's stdin. In particular the helper must not retain the DRI
           framebuffer descriptor owned by the supervisor. */
        close_extra_fds(input_fd, sh.to_shell, -1, -1);
        standalone_input_pump(input_fd, sh.to_shell, sh.pid);
        sys_exit(0);
    }
    /* Supervisor/renderer owns the display and the shell's output. */
    close(sh.to_shell);

    term_init(&term, (int)fb.w / FONT_WIDTH,
              ((int)fb.h - 24 - 8) / FONT_HEIGHT);
    axgui_fill(&fb, 0, 0, (int)fb.w, (int)fb.h, C_BG);
    axgui_fill(&fb, 0, 0, (int)fb.w, 24, C_BAR);
    axgui_text(&fb, "axterm - /bin/sh", 8, 4, C_FG, C_BAR);
    term_draw(&fb, &term, 8, 28);
    axgui_present(&fb);

    for (;;)
    {
        long n = read(sh.from_shell, out, sizeof(out));
        if (n > 0)
        {
            term_feed(&term, out, (size_t)n);
            axgui_fill(&fb, 0, 0, (int)fb.w, 24, C_BAR);
            axgui_text(&fb, "axterm - /bin/sh", 8, 4, C_FG, C_BAR);
            term_draw(&fb, &term, 8, 28);
            axgui_present(&fb);
        }
        else if (n == 0)
        {
            out_eof = 1;
        }
        else
        {
            out_eof = 1;
        }
        if (helper > 0 && !helper_gone)
        {
            long hq;
            errno = 0;
            hq = sys_waitpid_nb((int)helper, 0);
            if (hq == helper || (hq < 0 && errno == ECHILD))
                helper_gone = 1;
        }
        if (!shell_gone)
        {
            int st = 0;
            long q;
            errno = 0;
            q = sys_waitpid_nb((int)sh.pid, &st);
            if (q == sh.pid)
            {
                shell_gone = 1;
                shell_status = st;
            }
            else if (q < 0 && errno == ECHILD)
            {
                shell_gone = 1;
            }
        }
        if (helper_gone && !shell_gone)
            kill_process_tree(sh.pid);
        if (out_eof && shell_gone)
            break;
        if (out_eof && !shell_gone)
        {
            kill_process_tree(sh.pid);
            if (reap_child_deadline(sh.pid, &shell_status, 2000) == 0)
            {
                shell_gone = 1;
                break;
            }
            axgui_msleep(10);
        }
    }

    if (helper > 0)
    {
        /* The standalone helper polls with syscalls, so SIGKILL reaches it
           promptly and the blocking reap below is safe. */
        kill(helper, SIGKILL);
        sys_waitpid((int)helper, 0);
    }
    close(sh.from_shell);
    if (input_fd >= 0)
    {
        axgui_grab(input_fd, 0);
        close(input_fd);
    }
    axgui_close(&fb);
    printf("\n[axterm closed]\n");
    return shell_status;

stand_fail:
    if (input_fd >= 0)
    {
        axgui_grab(input_fd, 0);
        close(input_fd);
    }
    axgui_close(&fb);
    return 1;
}

int main(int argc, char **argv)
{
    /* Compositor client: `axterm --wm <shmid> <w> <h> <evfd> [<gen>]
       [--user <uid> <gid>] [-c cmd...]`. */
    if (argc >= 6 && strcmp(argv[1], "--wm") == 0)
    {
        struct axclient cx;
        char **cargv = 0;
        int cargc = 0;
        int uid = -1;
        int gid = -1;
        int rest = axclient_init(&cx, argc, argv, "axterm");
        long u, g;
        if (rest < 0)
            return 1;
        if (argc > rest + 2 && strcmp(argv[rest], "--user") == 0 &&
            axclient_parse_num(argv[rest + 1], &u) == 0 && u >= 0 &&
            axclient_parse_num(argv[rest + 2], &g) == 0 && g >= 0)
        {
            uid = (int)u;
            gid = (int)g;
            rest += 3;
        }
        if (argc > rest + 1 && strcmp(argv[rest], "-c") == 0 &&
            argc > rest + 2)
        {
            cargv = &argv[rest + 1];
            cargc = argc - (rest + 1);
        }
        return run_wm_client(&cx, cargv, cargc, uid, gid);
    }

    return run_standalone();
}
