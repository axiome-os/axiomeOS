/* axterm — graphical terminal for axiomeOS (/Binaries/axterm).
   Two modes:
   * standalone (no args): fullscreen /Devices/dri0 terminal, holds the GUI
     input grab (like axwm does).
   * compositor client (`axterm --wm <shmid> <w> <h> <evfd> [-c cmd...]`):
   *   draws into the axwm window SHM segment and reads wm_events from
   *   <evfd> (pipe read end inherited across spawn). This is the binary
   *   axwm launches for its Terminal windows.
   *
   * Type commands at the "$ " prompt; Enter runs them with stdout captured
   * into the scrollback (one-shot commands: ls, cat, uname, ps, ...).
   * Builtins: help, clear, exit, gui (standalone only), cd, pwd, echo. */

#include "axgui.h"

#define HIST_LINES 256
#define MAX_COLS 256
#define IN_MAX 512
#define CMD_HIST 16
#define OUT_CAP 8192

/* Palette (canonical 0x00RRGGBB). */
#define C_BG      0x101418u
#define C_BAR     0x1E2836u
#define C_FG      0xD8DEE9u
#define C_PROMPT  0x88C0D0u
#define C_ACCENT  0x5E81ACu
#define C_DIM     0x4C566Au

static char g_lines[HIST_LINES][MAX_COLS + 1];
static int g_nlines;
static char g_input[IN_MAX];
static int g_ilen;
static int g_cursor;
static char g_cmdhist[CMD_HIST][IN_MAX];
static int g_hcount;
static int g_hpos;
static char g_cwd[256];
static int g_wm_mode;

static void term_line(const char *s)
{
    int i;
    if (g_nlines >= HIST_LINES)
    {
        for (i = 0; i < HIST_LINES - 1; i++)
            strcpy(g_lines[i], g_lines[i + 1]);
        g_nlines = HIST_LINES - 1;
    }
    strncpy(g_lines[g_nlines], s, MAX_COLS);
    g_lines[g_nlines][MAX_COLS] = 0;
    g_nlines++;
}

/* Append captured output: split lines, expand tabs, wrap at `cols`. */
static void term_put(const char *s, int cols)
{
    char cur[MAX_COLS + 1];
    int n = 0;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    if (cols < 8)
        cols = 8;
    while (*s)
    {
        char c = *s++;
        if (c == '\r')
            continue;
        if (c == '\n')
        {
            cur[n] = 0;
            term_line(cur);
            n = 0;
            continue;
        }
        if (c == '\t')
        {
            do {
                if (n < cols)
                    cur[n++] = ' ';
            } while ((n % 8) && n < cols);
        }
        else if ((unsigned char)c >= 32 || (unsigned char)c > 126)
        {
            if (n < cols)
                cur[n++] = c;
        }
        if (n >= cols)
        {
            cur[n] = 0;
            term_line(cur);
            n = 0;
        }
    }
    if (n > 0)
    {
        cur[n] = 0;
        term_line(cur);
    }
}

static void add_cmdhist(const char *cmd)
{
    int i;
    if (!cmd[0])
        return;
    if (g_hcount > 0 && strcmp(g_cmdhist[g_hcount - 1], cmd) == 0)
        return;
    if (g_hcount < CMD_HIST)
    {
        strcpy(g_cmdhist[g_hcount++], cmd);
    }
    else
    {
        for (i = 0; i < CMD_HIST - 1; i++)
            strcpy(g_cmdhist[i], g_cmdhist[i + 1]);
        strcpy(g_cmdhist[CMD_HIST - 1], cmd);
    }
    g_hpos = g_hcount;
}

static void refresh_cwd(void)
{
    if (getcwd(g_cwd, sizeof(g_cwd)) != 0)
        strcpy(g_cwd, "/");
}

static void do_echo(const char *cmd)
{
    const char *p = cmd + 4;
    while (*p == ' ' || *p == '\t')
        p++;
    term_put(p, MAX_COLS);
}

static void do_help(void)
{
    term_put("axterm - graphical terminal (Mesa/dri0 backend)", MAX_COLS);
    term_put("type any command; output is captured above.", MAX_COLS);
    term_put("builtins: help clear exit gui cd pwd echo", MAX_COLS);
}

static void run_external(const char *cmd, int cols)
{
    static char out[OUT_CAP];
    long r = axgui_run_capture(cmd, out, sizeof(out));
    if (r < 0)
    {
        char msg[IN_MAX + 32];
        const char *name = cmd;
        int i = 0;
        while (name[i] == ' ' || name[i] == '\t')
            name++;
        snprintf(msg, sizeof(msg), "axterm: '%s': command not found", name);
        term_put(msg, cols);
        return;
    }
    if (r > 0)
        term_put(out, cols);
}

static void exec_line(const char *cmd, int cols, int *exit_req)
{
    char word[64];
    int i = 0;
    while (cmd[i] == ' ' || cmd[i] == '\t')
        i++;
    {
        int k = 0;
        while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && k < 63)
            word[k++] = cmd[i++];
        word[k] = 0;
    }
    if (word[0] == 0)
        return;
    if (strcmp(word, "exit") == 0)
    {
        *exit_req = 1;
    }
    else if (strcmp(word, "clear") == 0)
    {
        g_nlines = 0;
    }
    else if (strcmp(word, "help") == 0)
    {
        do_help();
    }
    else if (strcmp(word, "echo") == 0)
    {
        do_echo(cmd);
    }
    else if (strcmp(word, "cd") == 0)
    {
        const char *p = cmd;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            p = "/";
        if (chdir(p) < 0)
        {
            char msg[300];
            snprintf(msg, sizeof(msg), "cd: %s: no such directory", p);
            term_put(msg, cols);
        }
        else
        {
            refresh_cwd();
        }
    }
    else if (strcmp(word, "pwd") == 0)
    {
        refresh_cwd();
        term_put(g_cwd, cols);
    }
    else if (strcmp(word, "gui") == 0)
    {
        if (g_wm_mode)
        {
            term_put("already running under axwm", cols);
        }
        else
        {
            term_put("starting axwm ...", cols);
            run_external("/bin/axwm", cols);
        }
    }
    else
    {
        run_external(cmd, cols);
    }
}

static void handle_key(uint32_t code, int cols, int *exit_req,
                       int input_fd)
{
    int i;
    (void)input_fd;
    if (code == '\n' || code == '\r')
    {
        char prompt[IN_MAX + 8];
        g_input[g_ilen] = 0;
        snprintf(prompt, sizeof(prompt), "$ %s", g_input);
        term_put(prompt, cols);
        add_cmdhist(g_input);
        exec_line(g_input, cols, exit_req);
        g_ilen = 0;
        g_cursor = 0;
        g_input[0] = 0;
        return;
    }
    if (code == 127 || code == 8)
    {
        if (g_cursor > 0)
        {
            for (i = g_cursor - 1; i < g_ilen - 1; i++)
                g_input[i] = g_input[i + 1];
            g_ilen--;
            g_cursor--;
            g_input[g_ilen] = 0;
        }
        return;
    }
    if (code == 27)
    {
        g_ilen = 0;
        g_cursor = 0;
        g_input[0] = 0;
        return;
    }
    if (code == AXINPUT_KEY_UP)
    {
        if (g_hpos > 0)
        {
            g_hpos--;
            strcpy(g_input, g_cmdhist[g_hpos]);
            g_ilen = strlen(g_input);
            g_cursor = g_ilen;
        }
        return;
    }
    if (code == AXINPUT_KEY_DOWN)
    {
        if (g_hpos < g_hcount - 1)
        {
            g_hpos++;
            strcpy(g_input, g_cmdhist[g_hpos]);
            g_ilen = strlen(g_input);
            g_cursor = g_ilen;
        }
        else
        {
            g_hpos = g_hcount;
            g_ilen = 0;
            g_cursor = 0;
            g_input[0] = 0;
        }
        return;
    }
    if (code == AXINPUT_KEY_LEFT)
    {
        if (g_cursor > 0)
            g_cursor--;
        return;
    }
    if (code == AXINPUT_KEY_RIGHT)
    {
        if (g_cursor < g_ilen)
            g_cursor++;
        return;
    }
    if (code == AXINPUT_KEY_HOME)
    {
        g_cursor = 0;
        return;
    }
    if (code == AXINPUT_KEY_END)
    {
        g_cursor = g_ilen;
        return;
    }
    if (code >= 32 && code < 127 && g_ilen < IN_MAX - 1)
    {
        for (i = g_ilen; i > g_cursor; i--)
            g_input[i] = g_input[i - 1];
        g_input[g_cursor] = (char)code;
        g_ilen++;
        g_cursor++;
        g_input[g_ilen] = 0;
    }
}

static void render(struct axgui_fb *fb, int tick)
{
    int cols, rows, i, y;
    char bar[256];
    char prompt[IN_MAX + 8];
    cols = (int)fb->w / FONT_WIDTH;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    rows = ((int)fb->h - 24 - FONT_HEIGHT - 6) / FONT_HEIGHT;
    if (rows < 4)
        rows = 4;

    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, C_BG);
    /* Title bar. */
    axgui_fill(fb, 0, 0, (int)fb->w, 24, C_BAR);
    refresh_cwd();
    snprintf(bar, sizeof(bar), "axterm  %s   [Esc clears line]",
             g_cwd);
    axgui_text(fb, bar, 8, 4, C_FG, C_BAR);

    /* Scrollback: last `rows` lines. */
    y = 24 + 4;
    {
        int start = g_nlines - rows;
        if (start < 0)
            start = 0;
        for (i = start; i < g_nlines; i++)
        {
            axgui_text_cell(fb, g_lines[i], cols, 8, y, C_FG, C_BG);
            y += FONT_HEIGHT;
        }
    }
    /* Input line. */
    snprintf(prompt, sizeof(prompt), "$ %s", g_input);
    axgui_text_cell(fb, prompt, cols, 8, (int)fb->h - FONT_HEIGHT - 6,
                    C_PROMPT, C_BG);
    /* Block cursor (blink 2 Hz). */
    if ((tick / 15) % 2 == 0)
    {
        int cx = 8 + (2 + g_cursor) * FONT_WIDTH;
        int cy = (int)fb->h - FONT_HEIGHT - 6;
        axgui_fill(fb, cx, cy, FONT_WIDTH, FONT_HEIGHT, C_ACCENT);
        if (g_cursor < g_ilen)
            axgui_glyph(fb, g_input[g_cursor], cx, cy, C_BG, C_ACCENT);
    }
    /* Bottom hint. */
    axgui_text(fb, "axterm: type 'help' | 'exit' returns to console",
               8, (int)fb->h - FONT_HEIGHT - 6 + FONT_HEIGHT, C_DIM, C_BG);
    (void)tick;
}

/* ---- compositor client mode (`axterm --wm ...`) ---- */

static void render_content(struct axgui_fb *fb)
{
    int cols, rows, i, y;
    char prompt[IN_MAX + 8];
    cols = (int)fb->w / FONT_WIDTH;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    rows = ((int)fb->h - FONT_HEIGHT) / FONT_HEIGHT;
    if (rows < 2)
        rows = 2;

    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, C_BG);
    y = 4;
    {
        int start = g_nlines - rows;
        if (start < 0)
            start = 0;
        for (i = start; i < g_nlines; i++)
        {
            axgui_text_cell(fb, g_lines[i], cols, 8, y, C_FG, C_BG);
            y += FONT_HEIGHT;
        }
    }
    snprintf(prompt, sizeof(prompt), "$ %s", g_input);
    axgui_text_cell(fb, prompt, cols, 8, (int)fb->h - FONT_HEIGHT - 2,
                    C_PROMPT, C_BG);
    /* Static block cursor (the client has no timer; it redraws on events). */
    {
        int cx = 8 + (2 + g_cursor) * FONT_WIDTH;
        int cy = (int)fb->h - FONT_HEIGHT - 2;
        axgui_fill(fb, cx, cy, FONT_WIDTH, FONT_HEIGHT, C_ACCENT);
        if (g_cursor < g_ilen)
            axgui_glyph(fb, g_input[g_cursor], cx, cy, C_BG, C_ACCENT);
    }
}

/* Join argv[0..n) with spaces (for the -c initial command). */
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

static int run_wm_client(long shmid, int req_w, int req_h, int evfd,
                         char **argv, int argc)
{
    struct axgui_fb cfb;
    struct wm_win_hdr *hdr;
    int cols, exit_req = 0;
    (void)req_w;
    (void)req_h;

    g_wm_mode = 1;
    printf("axterm-wm: attaching shm %ld evfd %d\n", shmid, evfd);
    hdr = axgui_win_attach(shmid, &cfb);
    if (!hdr)
    {
        printf("axterm-wm: attach failed\n");
        return 1;
    }
    printf("axterm-wm: attached %ux%u, entering loop\n", cfb.w, cfb.h);
    cols = (int)cfb.w / FONT_WIDTH;
    if (cols > MAX_COLS)
        cols = MAX_COLS;

    refresh_cwd();
    term_put("axterm - terminal client (type 'help')", cols);
    if (argc > 0)
    {
        char cmd[IN_MAX];
        char echo[IN_MAX + 4];
        join_args(argv, argc, cmd, sizeof(cmd));
        if (cmd[0])
        {
            snprintf(echo, sizeof(echo), "$ %s", cmd);
            term_put(echo, cols);
            add_cmdhist(cmd);
            exec_line(cmd, cols, &exit_req);
            if (exit_req)
                return 0;
        }
    }
    render_content(&cfb);
    hdr->seq++;
    hdr->ready = 1;

    for (;;)
    {
        struct wm_event ev;
        if (hdr->closed)
        {
            printf("axterm-wm: closed by compositor\n");
            break;
        }
        if (axgui_wm_recv(evfd, &ev) < 0)
        {
            printf("axterm-wm: event channel EOF/err\n");
            break; /* compositor went away */
        }
        if (ev.type != WM_EV_KEY)
            continue; /* mouse is decorative for a terminal */
        handle_key(ev.code, cols, &exit_req, -1);
        if (exit_req)
            break;
        render_content(&cfb);
        hdr->seq++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct axgui_fb fb;
    int input_fd;
    int exit_req = 0;
    int tick = 0;
    struct axinput_event ev[64];
    (void)argv;

    /* Compositor client: `axterm --wm <shmid> <w> <h> <evfd> [-c cmd...]`. */
    if (argc >= 6 && strcmp(argv[1], "--wm") == 0)
    {
        long shmid = atol(argv[2]);
        int w = atoi(argv[3]);
        int h = atoi(argv[4]);
        int evfd = atoi(argv[5]);
        int ci = 0;
        char **cargv = 0;
        if (argc > 7 && strcmp(argv[6], "-c") == 0 && argc > 8)
        {
            ci = argc - 7;
            cargv = &argv[7];
        }
        return run_wm_client(shmid, w, h, evfd, cargv, ci);
    }

    fb.fd = -1;
    if (axgui_dri_open(&fb) < 0)
    {
        printf("axterm: no DRI device (need GOP framebuffer)\n");
        return 1;
    }
    input_fd = axgui_input_open();
    if (input_fd >= 0)
        axgui_grab(input_fd, 1);

    refresh_cwd();
    term_put("axterm v1.0 - graphical terminal (type 'help')", 80);
    /* Drain stale keys pressed while the shell prompt was up. */
    if (input_fd >= 0)
        axgui_poll(input_fd, ev, 64);

    for (;;)
    {
        int n, i;
        int cols = (int)fb.w / FONT_WIDTH;
        if (cols > MAX_COLS)
            cols = MAX_COLS;
        if (input_fd >= 0)
        {
            n = axgui_poll(input_fd, ev, 64);
            for (i = 0; i < n; i++)
            {
                if (ev[i].type != AXINPUT_TYPE_KEY)
                    continue;
                handle_key(ev[i].code, cols, &exit_req, input_fd);
                if (exit_req)
                    break;
            }
            if (exit_req)
                break;
        }
        else
        {
            sys_yield();
        }
        render(&fb, tick++);
        axgui_present(&fb);
        axgui_msleep(33);
    }

    if (input_fd >= 0)
    {
        axgui_grab(input_fd, 0);
        close(input_fd);
    }
    axgui_close(&fb);
    printf("\n[axterm closed]\n");
    return 0;
}
