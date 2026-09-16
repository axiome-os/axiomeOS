/* guixd — axiome display server daemon ("GUI X daemon").
   Runs as a system service spawned by axiome-init, so it inherits the
   SYSTEM role and is one of the very few processes allowed to talk to the
   graphics/input devices directly (/Devices/dri0 + /Devices/input0; the
   kernel gates them to ROLE_SYSTEM in sys_open). Regular users get the
   desktop through this daemon instead of raw hardware access.
   Clients are real programs from /Binaries (axterm, axinfo, ...): each
   window slot owns an SHM segment (see wm_abi.h) that the client attaches
   and draws into; input reaches the focused client as wm_events over a
   per-client pipe (read end inherited, number passed on argv). Child exits are reaped via SYS_WAITPID_NB.

   Render loop (performance): the desktop is NOT redrawn at a fixed rate.
   A full software render + PRESENT happens only when state that affects
   pixels changed (input other than plain pointer motion, a client repaint
   via its hdr->seq, a window spawn/exit, the 1 Hz clock). While the mouse
   just moves, the previous cursor is erased from the staging buffer
   (cursor snapshot), the new one drawn, and only the two small cursor
   rects are PRESENTed. Idle frames do nothing but poll + sleep, so the
   text console ("tty thing") and log writers no longer force 20 fps
   full-screen blits while the desktop is open. */

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

struct win {
    int used;
    int live;      /* ready seen: composited and eligible for input */
    int closing;   /* close requested: freed silently on reap, no dead frame */
    int dead;      /* client reaped; frame kept until dismissed */
    int x, y; /* frame origin; size fixed (WIN_FW x WIN_FH) */
    char title[48];
    int focused;
    int dragging;
    int drag_ox, drag_oy;
    long shmid;
    struct wm_win_hdr *hdr; /* compositor mapping of the window SHM */
    long pid;               /* client pid, -1 when none/reaped */
    int evfd;               /* event pipe write end, -1 when closed */
    int status;
    int chain_login;        /* when this client exits 0, open the login window */
    int is_login;           /* axlogin: a status-0 reap spawns its session */
    uint32_t gen;           /* slot generation handed to the client */
    long ready_deadline;    /* time(0) by which ready must appear */
    long close_deadline;    /* time(0) when SIGKILL escalates a close */
    int last_mx, last_my;   /* last mouse state forwarded */
    uint32_t last_btn;
    uint32_t seq;           /* last seqlock seq fully blitted (repaint trigger) */
    int fullscreen;         /* fill entire screen, no title bar/border/close */
    int noclose;            /* cannot be closed via X button */
};

static struct win g_wins[WM_MAX_WIN];
static int g_z[WM_MAX_WIN];
static int g_nz;
static int g_menu;
static int g_mx, g_my;
static uint32_t g_btn;
static int g_drag_win = -1;
static char g_apps[16][64];
static int g_napps;
static uint32_t g_gencnt;    /* slot generation counter (never 0, < 2^31) */
static int g_queued_login;   /* open the login window after the reap scan */
static long g_gfx_pid = -1;  /* async fullscreen demo pid, -1 when none */
static long g_empty_since = -1; /* time(0) when the desktop last went empty */
static int g_spawn_fails;    /* consecutive auto-reopen failures (backoff) */
static long g_sess_uid = -1; /* logged-in user: later terminals inherit it */
static long g_sess_gid = -1; /* (-1 = none yet: terminals stay as spawned) */

/* Cursor snapshot: pixels under the pointer captured at the last full render,
   so a pointer-only move can erase the old cursor without a full re-render. */
static uint32_t g_cursor_save[CURS_W * CURS_H];
static int g_cursor_save_ok;
static int g_cursor_x, g_cursor_y;

static void cursor_snapshot(struct axgui_fb *fb, int x, int y)
{
    int r, c;
    if (!fb || !fb->px)
        return;
    for (r = 0; r < CURS_H; r++)
        for (c = 0; c < CURS_W; c++)
        {
            int px = x + c, py = y + r;
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
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
            if (px < 0 || py < 0 || (uint32_t)px >= fb->w || (uint32_t)py >= fb->h)
                continue;
            fb->px[(size_t)py * (fb->pitch / 4) + (size_t)px] =
                g_cursor_save[(size_t)r * CURS_W + c];
        }
}

static int has_fullscreen(void)
{
    int i;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        if (g_wins[i].used && !g_wins[i].dead && g_wins[i].fullscreen)
            return 1;
    return 0;
}

static struct win *win_at(int x, int y)
{
    int i;
    for (i = g_nz - 1; i >= 0; i--)
    {
        struct win *w = &g_wins[g_z[i]];
        if (!w->dead && w->fullscreen) return w;
        if (w->dead && w->fullscreen) continue;
        if (x >= w->x && x < w->x + (int)WIN_FW && y >= w->y &&
            y < w->y + (int)WIN_FH)
            return w;
    }
    return 0;
}

static void win_focus(struct win *w)
{
    int i, k;
    if (!w || !w->used || w->dead)
        return; /* never focus a corpse or a free slot */
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
        if (g_wins[i].used && g_wins[i].focused && !g_wins[i].dead)
            return &g_wins[i];
    return 0;
}

/* Focus the topmost live window (deterministic rule: always the top).
   Called whenever focus could be left pointing at a corpse. */
static void win_elect_focus(void)
{
    int i;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        g_wins[i].focused = 0;
    for (i = g_nz - 1; i >= 0; i--)
    {
        struct win *t = &g_wins[g_z[i]];
        if (t->used && !t->dead)
        {
            t->focused = 1;
            break;
        }
    }
}

/* Ask a client to exit without stalling the frame loop: flag it, close
   the event pipe first so a well-behaved client sees EOF promptly, and
   record a SIGKILL deadline. The reap scan finishes the job (SIGKILL on
   expiry, silent free for requested closes, dead frame for crashes). */
static void win_request_close(struct win *w, long now)
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
    if (w->dragging || g_drag_win == (int)(w - g_wins))
    {
        w->dragging = 0;
        if (g_drag_win == (int)(w - g_wins))
            g_drag_win = -1;
    }
    if (!w->closing)
    {
        w->closing = 1;
        w->close_deadline = now + 1;
    }
}

/* Free a slot whose client is already reaped (pid == -1 only: a live child
   must never become untracked). Re-elects focus so input never sticks on
   the freed window. */
static void win_free_slot(struct win *w)
{
    int i, k;
    if (w->pid > 0)
        return;
    w->used = 0;
    w->live = 0;
    w->closing = 0;
    w->dead = 0;
    w->dragging = 0;
    w->fullscreen = 0;
    w->noclose = 0;
    w->chain_login = 0;
    w->is_login = 0;
    if (g_drag_win == (int)(w - g_wins))
        g_drag_win = -1;
    k = 0;
    for (i = 0; i < g_nz; i++)
    {
        if (&g_wins[g_z[i]] != w)
            g_z[k++] = g_z[i];
    }
    g_nz = k;
    win_elect_focus();
}

/* Build the client command line. A non-negative uid/gid appends a --user
   segment (before -c, matching the clients' argv parsing). 0 on success,
   -1 when the line would not fit: a truncated shmid/evfd digit would
   attach the client to the wrong window, so we never spawn it. */
static int build_client_cmd(char *cmd, size_t cap, const char *prog,
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
                     shmid, WM_WIN_W, WM_WIN_H, evfd, gen, user, initial);
    else
        r = snprintf(cmd, cap, "%s --wm %ld %u %u %d %u%s", prog, shmid,
                     WM_WIN_W, WM_WIN_H, evfd, gen, user);
    if (r < 0 || (size_t)r >= cap)
        return -1;
    return 0;
}

/* (Re)open a client in slot w and take focus. The slot must be reserved
   (used==1) with no live client (pid==-1, evfd==-1): fresh opens reserve
   it first, session reuse inherits the reaped login's reservation.
   Returns w, or 0 (slot left reserved but empty: the caller frees it). */
static struct win *open_client_in(struct win *w, const char *title,
                                  const char *prog, const char *initial,
                                  long uid, long gid, struct axgui_fb *fb)
{
    int fds[2];
    char cmd[224];
    long pid;
    int i;
    size_t pxn, i2;
    int k;
    if (!w || !w->used || w->pid > 0 || w->evfd >= 0 || !w->hdr)
        return 0;
    if (pipe(fds) < 0)
        return 0;
    if (++g_gencnt == 0 || g_gencnt > 0x7FFFFFFFu)
        g_gencnt = 1;
    /* Fresh pixels + header for the new client, pre-spawn (see above). */
    pxn = (size_t)WM_WIN_W * WM_WIN_H;
    for (i2 = 0; i2 < pxn; i2++)
        ((uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE))[i2] = D_WinBG;
    w->hdr->magic = WM_MAGIC;
    w->hdr->w = WM_WIN_W;
    w->hdr->h = WM_WIN_H;
    w->hdr->closed = 0;
    w->hdr->seq = 0;
    w->hdr->ready = 0;
    w->hdr->gen = g_gencnt;
    /* The read end travels by inheritance; its number goes on the command
       line (no dup2 onto a fixed fd: the target might be one of the pipe's
       own ends, and dup2 would close it first). */
    if (build_client_cmd(cmd, sizeof(cmd), prog, w->shmid, fds[0], g_gencnt,
                         initial, uid, gid) < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    pid = sys_spawn_cmd(cmd, strlen(cmd));
    close(fds[0]); /* guixd's alias; the child keeps its own */
    if (pid < 0)
    {
        close(fds[1]);
        return 0;
    }
    printf("guixd: spawned '%s' pid=%ld evfd=%d shm=%ld gen=%u\n", prog, pid,
           fds[1], w->shmid, g_gencnt);

    memset(w->title, 0, sizeof(w->title));
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->live = 0;
    w->closing = 0;
    w->dead = 0;
    w->focused = 1;
    w->dragging = 0;
    w->pid = pid;
    w->evfd = fds[1];
    w->status = 0;
    w->chain_login = 0;
    w->is_login = 0;
    w->gen = g_gencnt;
    w->ready_deadline = (long)time(0) + 4;
    w->close_deadline = 0;
    w->last_mx = -1;
    w->last_my = -1;
    w->last_btn = 0;
    w->seq = 0;
    /* To z-top (fresh append or reuse re-append: every used slot is in g_z
       exactly once, so there is always room). */
    k = 0;
    for (i = 0; i < g_nz; i++)
    {
        if (&g_wins[g_z[i]] != w)
            g_z[k++] = g_z[i];
    }
    g_z[k++] = (int)(w - g_wins);
    g_nz = k;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
        if (&g_wins[i] != w)
            g_wins[i].focused = 0;
    return w;
}

/* Launch a client program into a free window slot. `prog` must speak the
   wm_abi client protocol (argv: --wm <shmid> <w> <h> <evfd> <gen> ...).
   uid/gid >= 0 run the client as that user (--user); pass -1/-1 to keep
   the daemon's own credentials (login, setup, system info).

   Deterministic startup: the generation and the whole SHM header are
   initialised BEFORE the spawn, so the client can never observe a
   half-built window and the outcome no longer depends on who runs first.
   Like before, the new window takes focus (it lands on top, so keys going
   there match what the user sees). It becomes live once the client
   publishes its first frame (4 s timeout); until then it shows
   "starting ...", takes no input, and a hung starter is closed
   automatically instead of stranding the desktop. */
static struct win *open_client(const char *title, const char *prog,
                               const char *initial, long uid, long gid,
                               struct axgui_fb *fb)
{
    struct win *w = 0;
    int i;
    for (i = 0; i < (int)WM_MAX_WIN; i++)
    {
        if (!g_wins[i].used && g_wins[i].shmid > 0 && g_wins[i].hdr)
        {
            w = &g_wins[i];
            break;
        }
    }
    if (!w)
        return 0;
    w->used = 1;
    if (strcmp(prog, "axoobe") == 0 || strcmp(prog, "axlogin") == 0)
    {
        w->fullscreen = 1;
        w->noclose = 1;
    }
    if (w->fullscreen)
    {
        w->x = ((int)fb->w - (int)WM_WIN_W) / 2;
        w->y = ((int)fb->h - (int)WM_WIN_H) / 2;
    }
    else
    {
        w->x = 60 + (g_nz % 4) * 28;
        w->y = TOPBAR + 30 + (g_nz % 4) * 24;
    }
    if (w->x + (int)WIN_FW > (int)fb->w - 8)
        w->x = (int)fb->w - 8 - (int)WIN_FW;
    if (w->x < 0)
        w->x = 0;
    if (w->y + (int)WIN_FH > (int)fb->h - 8)
        w->y = (int)fb->h - 8 - (int)WIN_FH;
    if (w->y < TOPBAR)
        w->y = TOPBAR;
    if (!open_client_in(w, title, prog, initial, uid, gid, fb))
    {
        w->used = 0;
        return 0;
    }
    return w;
}

static struct win *open_terminal(const char *cmd, struct axgui_fb *fb)
{
    /* Terminals inherit the logged-in user once somebody logged in;
       before that they stay at the daemon's credentials. */
    struct win *w = open_client(cmd && cmd[0] ? cmd : "Terminal", "axterm",
                                cmd, g_sess_uid, g_sess_gid, fb);
    if (!w)
        printf("guixd: cannot launch axterm\n");
    return w;
}

/* Open the GUI login window (axlogin): authenticates, then the desktop
   spawns the user's terminal session in the same window. */
static struct win *open_login(struct axgui_fb *fb)
{
    struct win *w = open_client("Login", "axlogin", 0, -1, -1, fb);
    if (!w)
        printf("guixd: cannot launch axlogin\n");
    else
        w->is_login = 1;
    return w;
}

/* Open the GUI first-boot setup window (axoobe). */
static struct win *open_setup(struct axgui_fb *fb)
{
    struct win *w = open_client("First-boot setup", "axoobe", 0, -1, -1,
                                fb);
    if (!w)
        printf("guixd: cannot launch axoobe\n");
    return w;
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

/* True when /etc/passwd already holds a "user" or "admin" account (passwd
   format name:hash:uid:gid:role:dir:shell). */
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

/* First window(s) at boot: first-boot setup when no account exists yet
   (chained into login on success), otherwise the login window directly. */
static void boot_session(struct axgui_fb *fb)
{
    struct win *w;
    if (has_regular_user())
    {
        open_login(fb);
        return;
    }
    w = open_client("First-boot setup", "axoobe", 0, -1, -1, fb);
    if (!w)
    {
        printf("guixd: cannot launch axoobe\n");
        return;
    }
    w->chain_login = 1;
}

/* Session handoff from axlogin: on a status-0 reap it leaves
   /var/run/axlogin.<shmid> holding "<uid> <gid>\n". Consumed exactly once
   (unlinked even when unparsable, so nothing stale survives). */
#define GUIXD_SESS_FMT "/var/run/axlogin.%ld"

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

/* Replace a reaped login window with its authenticated session, reusing
   the slot (geometry, z-order handling and focus continuity come free).
   Records the session user: every terminal opened afterwards inherits it.
   On failure the slot keeps a dead frame (dismissable, never a loop). */
static struct win *win_reopen_session(struct win *w, long uid, long gid,
                                      struct axgui_fb *fb)
{
    if (!w || !w->used || w->pid > 0 || w->dead)
        return 0;
    w->status = 0;
    w->chain_login = 0;
    w->is_login = 0;
    w->fullscreen = 0;
    w->noclose = 0;
    if (!open_client_in(w, "Terminal", "axterm", 0, uid, gid, fb))
    {
        w->dead = 1;
        w->status = -1;
        win_elect_focus();
        return 0;
    }
    g_sess_uid = uid;
    g_sess_gid = gid;
    return w;
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

/* Fresh left-press edge. Returns 1 if consumed. `now` is time(0). */
static int handle_click(int x, int y, struct axgui_fb *fb, int input_fd,
                        long now)
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
            open_client("System info", "axinfo", 0, -1, -1, fb);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            open_login(fb);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            open_setup(fb);
            g_menu = 0;
            return 1;
        }
        if (idx == row++)
        {
            /* Fullscreen demo without freezing the desktop: ungrab input,
               spawn, and re-grab when the reap scan collects it. */
            g_menu = 0;
            if (g_gfx_pid <= 0)
            {
                long gp = sys_spawn_cmd("gfx_test", 8);
                if (gp > 0)
                {
                    g_gfx_pid = gp;
                    axgui_grab(input_fd, 0);
                }
            }
            return 1;
        }
        if (idx == row++)
        {
            g_menu = 0;
            sys_reboot();       /* full system restart (never returns) */
            return 1;
        }
        if (idx == row++)
        {
            g_menu = 0;
            sys_poweroff();     /* ACPI S5 power-off (never returns) */
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
        int xbtn = 0;
        int fullscreen;
        if (!w || !w->used)
            return 0;
        fullscreen = w->fullscreen;
        xbtn = !fullscreen && (x >= w->x + (int)WIN_FW - TITLEBAR &&
                x < w->x + (int)WIN_FW - 4 && y >= w->y + 3 &&
                y < w->y + TITLEBAR - 3);
        if (w->dead)
        {
            /* Dead frames only dismiss. */
            if (xbtn)
                win_free_slot(w);
            return 1;
        }
        /* Focus first, then deliver: the focusing press reaches the client
           instead of being swallowed, so one click always suffices. */
        win_focus(w);
        if (xbtn)
        {
            win_request_close(w, now);
            return 1;
        }
        if (!fullscreen && y >= w->y && y < w->y + TITLEBAR)
        {
            w->dragging = 1;
            w->drag_ox = x - w->x;
            w->drag_oy = y - w->y;
            g_drag_win = (int)(w - g_wins);
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
            if (lx >= 0 && ly >= 0 && lx < (int)WM_WIN_W &&
                ly < (int)WM_WIN_H)
            {
                struct wm_event me;
                me.type = WM_EV_MOUSE;
                me.code = g_btn;
                me.x = lx;
                me.y = ly;
                if (axgui_wm_send(w->evfd, &me) == 0)
                {
                    w->last_mx = lx;
                    w->last_my = ly;
                    w->last_btn = g_btn;
                }
                else
                {
                    win_request_close(w, now);
                }
            }
        }
        return 1;
    }
}

/* Copy one live window's pixels, seqlock-guarded (see wm_abi.h). Returns
   the consumed seq on success, -1 when skipped (writer active, torn copy,
   not ready): the caller then retries next frame instead of presenting a
   half-drawn client frame. On a torn copy the staging buffer keeps the
   D_WinBG fill render() laid down, so the window flickers to background
   for one frame rather than tearing. */
static long blit_content(struct axgui_fb *fb, struct win *w)
{
    uint32_t *src;
    uint32_t s0, s1;
    int r;
    if (!w->hdr || !w->live || w->dead || w->closing || !w->hdr->ready)
        return -1;
    s0 = w->hdr->seq;
    if (s0 & 1u)
        return -1; /* client mid-draw */
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
    __sync_synchronize();
    s1 = w->hdr->seq;
    if (s0 != s1)
        return -1; /* torn mid-copy: retry next frame */
    return (long)s0;
}

static void render(struct axgui_fb *fb)
{
    int i, x, y;
    char clock[32];
    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, D_BG);
    for (y = TOPBAR + 16; y < (int)fb->h; y += 32)
        for (x = 16; x < (int)fb->w; x += 32)
            axgui_px(fb, x, y, D_BG2);

    for (i = 0; i < g_nz; i++)
    {
        struct win *w = &g_wins[g_z[i]];
        if (w->dead && w->fullscreen)
            continue;
        uint32_t tc = w->focused ? D_TitleF : D_Title;
        if (w->fullscreen && !w->dead)
        {
            int cx = ((int)fb->w - (int)WM_WIN_W) / 2;
            int cy = ((int)fb->h - (int)WM_WIN_H) / 2;
            uint32_t *src;
            uint32_t s0, s1;
            int r, c;
            axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, D_WinBG);
            if (w->hdr && w->hdr->ready && !w->dead)
            {
                s0 = w->hdr->seq;
                if (!(s0 & 1u))
                {
                    src = (uint32_t *)((uint8_t *)w->hdr + WM_HDR_SIZE);
                    for (r = 0; r < (int)WM_WIN_H; r++)
                    {
                        uint32_t *drow =
                            &fb->px[(size_t)(cy + r) * (fb->pitch / 4) + (size_t)(cx)];
                        uint32_t *srow = &src[(size_t)r * WM_WIN_W];
                        for (c = 0; c < (int)WM_WIN_W; c++)
                        {
                            if (cx + c < 0 || cx + c >= (int)fb->w ||
                                cy + r < 0 || cy + r >= (int)fb->h)
                                continue;
                            drow[c] = srow[c];
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
                axgui_fill(fb, cx, cy, (int)WM_WIN_W, (int)WM_WIN_H,
                           D_WinBG);
                snprintf(note, sizeof(note), "starting ...");
                axgui_text(fb, note, cx + 8, cy + 8, D_Dim, D_WinBG);
            }
            continue;
        }
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
            long s = blit_content(fb, w);
            if (s >= 0)
                w->seq = (uint32_t)s;
            /* s < 0: seqlock miss — w->seq stays stale so the repaint
               scan below re-renders promptly instead of tearing. */
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

    if (!has_fullscreen())
    {
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
    }
    if (!has_fullscreen() && g_menu)
    {
        int mh = (7 + g_napps + 1) * FONT_HEIGHT + 8;
        int my = TOPBAR + 2;
        int row = 0;
        axgui_fill(fb, 4, my, 240, mh, D_Menu);
        axgui_rect(fb, 4, my, 240, mh, D_Accent);
        axgui_text(fb, "+ New terminal", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_Prompt, D_Menu);
        axgui_text(fb, "= System info", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_FG, D_Menu);
        axgui_text(fb, "L Login", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_Prompt, D_Menu);
        axgui_text(fb, "F First-boot setup", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_FG, D_Menu);
        axgui_text(fb, "* gfx demo (fullscreen)", 12,
                   my + 4 + row++ * FONT_HEIGHT, D_FG, D_Menu);
        axgui_text(fb, "R Restart", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_Prompt, D_Menu);
        axgui_text(fb, "P Power off", 12, my + 4 + row++ * FONT_HEIGHT,
                   D_Close, D_Menu);
        for (i = 0; i < g_napps; i++)
        {
            char entry[80];
            snprintf(entry, sizeof(entry), "  %s", g_apps[i]);
            axgui_text(fb, entry, 12, my + 4 + row++ * FONT_HEIGHT, D_FG,
                       D_Menu);
        }
    }

    /* Capture the pixels under the pointer before painting it, so a later
       pointer-only move can erase this cursor via the snapshot. */
    cursor_snapshot(fb, g_mx, g_my);
    axgui_cursor(fb, g_mx, g_my);
}

/* Move the pointer with only the cursor rects on screen: erase the old
   cursor from the staging buffer, draw the new one, PRESENT just the
   union rect (no full-desktop re-render / full present). */
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
        axgui_present(fb); /* off-screen rect: fall back to full present */
}

int main(int argc, char **argv)
{
    struct axgui_fb fb;
    struct axinput_event ev[64];
    int input_fd;
    size_t win_bytes;
    int i;
    long last_clock = -1;
    (void)argc;
    (void)argv;

    fb.fd = -1;
    if (axgui_dri_open(&fb) < 0)
    {
        printf("guixd: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    input_fd = axgui_input_open();
    if (input_fd < 0)
    {
        printf("guixd: no input device (need input0)\n");
        axgui_close(&fb);
        return 1;
    }
    /* Grab the GUI input immediately: from here on the text console stops
       touching the framebuffer (the "tty thing" no longer competes with the
       desktop for the scanout). */
    axgui_grab(input_fd, 1);
    axgui_poll(input_fd, ev, 64);
    printf("guixd: display server up (%ux%u)\n", fb.w, fb.h);

    /* Fixed SHM pool: one window segment per slot, owned by the display
       server for its whole lifetime (no per-client churn, nothing to free). */
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
            printf("guixd: shm_create failed for slot %d\n", i);
            continue;
        }
        base = (uint8_t *)shm_attach(id);
        if (!base)
        {
            printf("guixd: shm_attach failed for slot %d\n", i);
            continue;
        }
        for (k = 0; k < win_bytes; k++)
            base[k] = 0;
        g_wins[i].shmid = id;
        g_wins[i].hdr = (struct wm_win_hdr *)base;
    }

    g_mx = (int)fb.w / 2;
    g_my = (int)fb.h / 2;
    g_cursor_save_ok = 0;
    scan_apps();
    boot_session(&fb);

    for (;;)
    {
        long now = (long)time(0);
        int state_changed = 0;  /* full re-render + present needed */
        int mouse_moved = 0;
        int motion_pending = 0; /* one coalesced motion forward per frame */
        int drained;

        /* Drain-until-empty (bounded per frame): every queued event is
           processed with per-event button edges, so a press+release inside
           one batch can never go invisible and clicks never depend on
           poll timing. */
        for (drained = 0; drained < 4; drained++)
        {
            int n = axgui_poll(input_fd, ev, 64);
            if (n <= 0)
                break;
            for (i = 0; i < n; i++)
            {
                if (ev[i].type == AXINPUT_TYPE_MOUSE)
                {
                    uint32_t pressed, released;
                    int nx = g_mx + ev[i].dx;
                    int ny = g_my + ev[i].dy;
                    if (nx < 0)
                        nx = 0;
                    if (ny < 0)
                        ny = 0;
                    if (nx >= (int)fb.w)
                        nx = (int)fb.w - 1;
                    if (ny >= (int)fb.h)
                        ny = (int)fb.h - 1;
                    if (nx != g_mx || ny != g_my)
                        mouse_moved = 1;
                    g_mx = nx;
                    g_my = ny;
                    pressed = ev[i].code & ~g_btn;
                    released = ~ev[i].code & g_btn;
                    g_btn = ev[i].code;
                    /* A held button can drag: treat as a layout change. */
                    if (g_btn & AXINPUT_BTN_LEFT)
                        state_changed = 1;
                    motion_pending = 1;
                    if (pressed & AXINPUT_BTN_LEFT)
                    {
                        if (handle_click(g_mx, g_my, &fb, input_fd, now))
                            state_changed = 1;
                    }
                    if (released & AXINPUT_BTN_LEFT)
                    {
                        if (g_drag_win >= 0 &&
                            g_drag_win < (int)WM_MAX_WIN)
                            g_wins[g_drag_win].dragging = 0;
                        g_drag_win = -1;
                        state_changed = 1;
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
                        if (f && f->live && !f->closing && f->pid > 0 &&
                            f->evfd >= 0)
                        {
                            struct wm_event ke;
                            ke.type = WM_EV_KEY;
                            ke.code = kc;
                            ke.x = 0;
                            ke.y = 0;
                            if (axgui_wm_send(f->evfd, &ke) < 0)
                                win_request_close(f, now);
                        }
                    }
                    /* Key events make the focused client repaint. */
                    state_changed = 1;
                }
            }
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
            }
        }

        /* Coalesced motion: one forward per frame to the window under the
           pointer (focused or not: hover must work before the first
           click). Drags already forced a full render above. */
        if (motion_pending)
        {
            struct win *hit = win_at(g_mx, g_my);
            if (hit && hit->used && !hit->dead && !hit->closing &&
                hit->pid > 0 && hit->evfd >= 0)
            {
                int lx, ly;
                if (hit->fullscreen)
                {
                    lx = g_mx - hit->x;
                    ly = g_my - hit->y;
                }
                else
                {
                    lx = g_mx - (hit->x + FRAME_X);
                    ly = g_my - (hit->y + FRAME_TOP);
                }
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
                    else
                    {
                        win_request_close(hit, now);
                    }
                }
            }
        }

        /* Lifecycle scan: reap the exited (never blocking), promote ready
           starters, time out hung ones, SIGKILL closes past deadline. */
        for (i = 0; i < (int)WM_MAX_WIN; i++)
        {
            struct win *w = &g_wins[i];
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
                    /* User-closed: vanish silently, no dead frame. */
                    win_free_slot(w);
                }
                else if (login_ok)
                {
                    /* Authenticated: swap this window for its session in
                       the same slot (spawned + tracked by us, never an
                       untracked grandchild). A broken handoff reopens
                       login instead of stranding or looping. */
                    long suid, sgid;
                    if (consume_session_file(w->shmid, &suid, &sgid) == 0 &&
                        win_reopen_session(w, suid, sgid, &fb))
                    {
                        printf("guixd: session started (uid=%ld gid=%ld)\n",
                               suid, sgid);
                    }
                    else
                    {
                        printf("guixd: bad session handoff, reopening login\n");
                        w->dead = 0;
                        if (open_client_in(w, "Login", "axlogin", 0, -1,
                                           -1, &fb))
                        {
                            w->is_login = 1;
                            w->fullscreen = 1;
                            w->noclose = 1;
                        }
                        else
                        {
                            w->dead = 1;
                            w->status = -1;
                            win_elect_focus();
                        }
                    }
                }
                else
                {
                    w->dead = 1;
                    w->status = st;
                    win_elect_focus();
                    if (chained)
                        g_queued_login = 1;
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
                    /* Never drew its first frame: close it; the reap
                       above then frees the slot. Never strands. */
                    printf("guixd: '%s' never became ready, closing\n",
                           w->title);
                    win_request_close(w, now);
                    state_changed = 1;
                }
            }
            else if (w->closing && now >= w->close_deadline)
            {
                kill((int)w->pid, SIGKILL);
            }
        }
        /* First-boot setup finished: chain into the login window
           automatically (after the scan: opening mutates the window list,
           which must not happen mid-scan). */
        if (g_queued_login)
        {
            g_queued_login = 0;
            open_login(&fb);
            state_changed = 1;
        }
        /* Async fullscreen demo: re-grab input and purge the backlog that
           queued while ungrabbed once the demo is reaped. */
        if (g_gfx_pid > 0)
        {
            int gst = 0;
            if (sys_waitpid_nb((int)g_gfx_pid, &gst) == g_gfx_pid)
            {
                g_gfx_pid = -1;
                axgui_grab(input_fd, 1);
                axgui_poll(input_fd, ev, 64);
                state_changed = 1;
            }
        }

        /* A live client published a frame since the last full render: pick
           it up promptly instead of waiting for the clock. Odd seq means
           mid-draw (picked up next frame); the blit itself re-validates. */
        for (i = 0; i < (int)WM_MAX_WIN; i++)
        {
            struct win *w = &g_wins[i];
            if (w->used && w->live && !w->dead && !w->closing && w->hdr &&
                w->hdr->ready && !(w->hdr->seq & 1u) &&
                w->hdr->seq != w->seq)
                state_changed = 1;
        }

        /* Desktop empty (no live or starting windows): reopen a terminal,
           but only after a beat and with backoff — a close stays closed
           for a moment, and a spawn failure can never become a respawn
           storm. */
        {
            int any = 0;
            long wait;
            for (i = 0; i < (int)WM_MAX_WIN; i++)
            {
                if (g_wins[i].used && !g_wins[i].dead)
                    any = 1;
            }
            if (!any)
            {
                wait = 2L << (g_spawn_fails > 4 ? 4 : g_spawn_fails);
                if (wait > 30)
                    wait = 30;
                if (g_empty_since < 0)
                    g_empty_since = now;
                if (now - g_empty_since >= wait)
                {
                    if (open_terminal(0, &fb))
                    {
                        g_spawn_fails = 0;
                        g_empty_since = -1;
                        state_changed = 1;
                    }
                    else
                    {
                        g_spawn_fails++;
                        g_empty_since = now;
                    }
                }
            }
            else
            {
                g_empty_since = -1;
                g_spawn_fails = 0;
            }
        }

        {
            if (now != last_clock)
            {
                last_clock = now;
                state_changed = 1;  /* refresh the topbar clock once per s */
            }
        }

        if (state_changed)
        {
            render(&fb);
            axgui_present(&fb);
        }
        else if (mouse_moved && g_cursor_save_ok)
        {
            redraw_cursor_only(&fb);
        }
        /* else: idle — no present, nothing pushed over PCIe. */

        axgui_msleep(16);
    }

    /* The daemon only leaves its loop via a system restart (menu "R
       Restart") or ACPI power-off (menu "P Power off") — both syscalls
       never return, so the loop above is the end of this process. */
    for (;;)
        sys_yield();
}