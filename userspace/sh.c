#include "stdio.h"
#include "syscall.h"
#include "signal.h"
#include "string.h"
#include "stdlib.h"

#define MAX_ARGS 32
#define LINE_MAX 256
#define HIST_SIZE 32

/* Builtin echo, mirroring the standalone tool (echo [-neE] [arg...]). */
static void echo_emit_plain(const char *s)
{
    while (*s)
        putchar((unsigned char)*s++);
}

static int echo_emit_escaped(const char *s)
{
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (c != '\\')
        {
            putchar(c);
            continue;
        }
        if (!*s)
        {
            putchar('\\');
            break;
        }
        unsigned char e = (unsigned char)*s++;
        switch (e)
        {
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'c': return 1;
        case 'e': putchar(27); break;
        case 'f': putchar('\f'); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
        {
            unsigned int v = (unsigned int)(e - '0');
            int n = 1;
            while (n < 3 && *s >= '0' && *s <= '7')
            {
                v = v * 8 + (unsigned int)(*s - '0');
                s++;
                n++;
            }
            putchar(v & 0xff);
            break;
        }
        case 'x':
        {
            unsigned int v = 0;
            int n = 0;
            while (n < 2 && *s &&
                   ((*s >= '0' && *s <= '9') ||
                    (*s >= 'a' && *s <= 'f') ||
                    (*s >= 'A' && *s <= 'F')))
            {
                unsigned int d;
                if (*s >= '0' && *s <= '9') d = (unsigned int)(*s - '0');
                else if (*s >= 'a' && *s <= 'f') d = 10u + (unsigned int)(*s - 'a');
                else d = 10u + (unsigned int)(*s - 'A');
                v = v * 16 + d;
                s++;
                n++;
            }
            putchar(v & 0xff);
            break;
        }
        default:
            putchar('\\');
            putchar(e);
            break;
        }
    }
    return 0;
}

static void sh_echo(int argc, char **argv)
{
    int no_nl = 0;
    int interpret = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0)
    {
        int only_opts = 1;
        for (const char *q = argv[i] + 1; *q; q++)
            if (*q != 'e' && *q != 'n' && *q != 'E')
            {
                only_opts = 0;
                break;
            }
        if (!only_opts)
            break;
        for (const char *q = argv[i] + 1; *q; q++)
        {
            if (*q == 'n') no_nl = 1;
            else if (*q == 'e') interpret = 1;
            else if (*q == 'E') interpret = 0;
        }
        i++;
    }
    int first = 1;
    int stop = 0;
    for (; i < argc; i++)
    {
        if (!first)
            putchar(' ');
        first = 0;
        if (interpret)
        {
            if (echo_emit_escaped(argv[i]))
            {
                stop = 1;
                break;
            }
        }
        else
        {
            echo_emit_plain(argv[i]);
        }
    }
    if (!no_nl && !stop)
        putchar('\n');
}

static char history[HIST_SIZE][LINE_MAX];
static int hist_count = 0;
static int hist_pos = 0;

/* Shell variables, set with `export NAME=value` and expanded as `$NAME`
   (issue #31: environment/variables in the shell). */
#define MAX_VARS 16
static char var_names[MAX_VARS][32];
static char var_vals[MAX_VARS][LINE_MAX];
static int var_count = 0;

static const char *var_lookup(const char *name)
{
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_names[i], name) == 0)
            return var_vals[i];
    return "";
}

/* Replace `$NAME` (names are [A-Za-z_][A-Za-z0-9_]*) with the variable value.
   This is a bounded, quote-agnostic pre-pass so it cannot overflow or corrupt
   the in-place tokenizer that follows. */
static void expand_vars(const char *src, char *dst, size_t cap)
{
    size_t di = 0;
    for (const char *p = src; *p && di + 1 < cap; )
    {
        if (*p == '$')
        {
            const char *q = p + 1;
            int nlen = 0;
            char nm[32];
            if (*q == '_' ||
                (*q >= 'a' && *q <= 'z') ||
                (*q >= 'A' && *q <= 'Z'))
            {
                nm[nlen++] = *q++;
                while (nlen < 31 &&
                       (*q == '_' ||
                        (*q >= 'a' && *q <= 'z') ||
                        (*q >= 'A' && *q <= 'Z') ||
                        (*q >= '0' && *q <= '9')))
                    nm[nlen++] = *q++;
                nm[nlen] = 0;
                const char *v = var_lookup(nm);
                while (*v && di + 1 < cap)
                    dst[di++] = *v++;
                p = q;
                continue;
            }
        }
        dst[di++] = *p++;
    }
    dst[di] = 0;
}

static char line[LINE_MAX];
static int line_len = 0;

static void add_history(const char *cmd)
{
    if (cmd[0] == 0) return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], cmd) == 0)
        return;
    if (hist_count < HIST_SIZE) {
        strcpy(history[hist_count], cmd);
        hist_count++;
    } else {
        for (int i = 0; i < HIST_SIZE - 1; i++)
            strcpy(history[i], history[i + 1]);
        strcpy(history[HIST_SIZE - 1], cmd);
    }
    hist_pos = hist_count;
}

static int read_line(char *buf, int max)
{
    int i = 0;
    char c;
    int esc = 0;
    
    for (;;) {
        c = getchar();
        if (c < 0) { sys_yield(); continue; }
        
        if (c == 27) { /* Escape */
            esc = 1;
            continue;
        }
        if (esc && c == '[') {
            esc = 2;
            continue;
        }
        if (esc == 2) {
            if (c == 'A') { /* Up */
                if (hist_pos > 0) {
                    hist_pos--;
                    strcpy(line, history[hist_pos]);
                    line_len = strlen(line);
                    printf("\r\033[K$ %s", line);
                }
            } else if (c == 'B') { /* Down */
                if (hist_pos < hist_count - 1) {
                    hist_pos++;
                    strcpy(line, history[hist_pos]);
                    line_len = strlen(line);
                    printf("\r\033[K$ %s", line);
                } else {
                    hist_pos = hist_count;
                    line_len = 0;
                    line[0] = 0;
                    printf("\r\033[K$ ");
                }
            }
            esc = 0;
            continue;
        }
        esc = 0;
        
        if (c == '\n' || c == '\r') {
            printf("\n");
            break;
        }
        if (c == 127 || c == 8) {
            if (i > 0) {
                i--;
                printf("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && i < max - 1) {
            buf[i++] = c;
            putchar(c);
        }
    }
    buf[i] = 0;
    if (buf[0]) {
        strcpy(line, buf);
        line_len = i;
        add_history(buf);
    }
    return i;
}

static int tokenize(char *s, char **argv)
{
    int argc = 0;
    char *p = s;
    while (*p && argc < MAX_ARGS)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        /* Quoting-aware scanning (issue #31): single quotes take everything
           literally; double quotes keep quoting until the matching quote and
           treat spaces inside as part of the token.

           `scan` walks the raw input while `dst` is where unquoted bytes are
           compacted. For an unquoted token they are identical, so terminating
           the token with *dst = 0 also overwrites the delimiter byte at *scan;
           we must remember we hit a delimiter (hit_delim) *before* that write
           and advance scan past it afterwards, or every token after the first
           is silently lost. */
        int in_dq = 0, in_sq = 0;
        char *dst = p;
        char *scan = p;
        int hit_delim = 0;
        while (*scan)
        {
            char ch = *scan;
            if (in_sq)
            {
                if (ch == '\'') in_sq = 0;
                else *dst++ = ch;
            }
            else if (in_dq)
            {
                if (ch == '"') in_dq = 0;
                else *dst++ = ch;
            }
            else if (ch == '\'')
            {
                in_sq = 1;
            }
            else if (ch == '"')
            {
                in_dq = 1;
            }
            else if (ch == ' ' || ch == '\t')
            {
                hit_delim = 1;
                break;
            }
            else
            {
                *dst++ = ch;
            }
            scan++;
        }
        *dst = 0;
        if (hit_delim)
        {
            scan++;
            while (*scan == ' ' || *scan == '\t') scan++;
        }
        p = scan;
    }
    return argc;
}

/* Split off an '&' background marker at the end of a command line.
   Returns 1 if the command should run in the background, 0 otherwise. */
static int parse_background(char *cmd)
{
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t'))
        n--;
    if (n > 0 && cmd[n - 1] == '&')
    {
        cmd[n - 1] = 0;
        return 1;
    }
    return 0;
}

/* Parse ">file", ">>file", or "<file" redirections. Sets up the shell's own
   fds (the spawned child inherits them via dup2) and rewrites `argv` in place
   to drop the redirection tokens *and* their filename arguments. Returns the
   new argc, or -1 on error (a failed open). */
static int do_redirect(char **argv, int argc)
{
    int dst = 0;
    for (int i = 0; i < argc; i++)
    {
        char *tok = argv[i];
        if (tok[0] == '>')
        {
            int append = (tok[1] == '>');
            const char *file = tok + (append ? 2 : 1);
            if (!*file && i + 1 < argc)
                file = argv[++i];   /* consume the filename token */
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            int f = open(file, flags);
            if (f < 0)
            {
                printf("sh: cannot open '%s'\n", file);
                return -1;
            }
            if (dup2(f, 1) < 0)
            {
                close(f);
                return -1;
            }
            close(f);
        }
        else if (tok[0] == '<')
        {
            const char *file = tok + 1;
            if (!*file && i + 1 < argc)
                file = argv[++i];   /* consume the filename token */
            int f = open(file, O_RDONLY);
            if (f < 0)
            {
                printf("sh: cannot open '%s'\n", file);
                return -1;
            }
            if (dup2(f, 0) < 0)
            {
                close(f);
                return -1;
            }
            close(f);
        }
        else
        {
            argv[dst++] = tok;
        }
    }
    return dst;
}

static void do_ps(void)
{
    struct proc_info procs[64];
    int n = sys_ps(procs, 64);
    if (n <= 0) {
        puts("ps: no processes");
        return;
    }
    const char *stname[4] = {"ready", "run", "block", "zomb"};
    puts("PID  STATE  NAME");
    for (int i = 0; i < n && i < 64; i++) {
        const char *sn = (procs[i].state >= 0 && procs[i].state < 4) ?
                         stname[procs[i].state] : "?";
        printf("%d   %s   %s\n", procs[i].pid, sn, procs[i].name);
    }
}

static void print_help(void)
{
    puts("");
    puts("axiome shell - builtins:");
    puts("  echo [-neE] <text>");
    puts("  ps");
    puts("  help");
    puts("  exit");
    puts("  cd <dir>");
    puts("  pwd");
    puts("  clear");
    puts("");
    puts("External: hello, cat, ls, mkdir, rm, touch, whoami, uname");
    puts("");
}

static void do_cd(char *path)
{
    if (chdir(path) < 0)
        printf("cd: %s: no such directory\n", path);
}

static void do_pwd(void)
{
    char buf[256];
    if (getcwd(buf, sizeof(buf)) == 0)
        printf("%s\n", buf);
    else
        puts("pwd: error");
}

static void do_kill(int argc, char **argv)
{
    if (argc < 3) {
        puts("usage: kill <pid> <sig>");
        return;
    }
    int pid = atoi(argv[1]);
    int sig = atoi(argv[2]);
    if (pid <= 0 || sig <= 0 || sig >= NSIG) {
        puts("kill: invalid");
        return;
    }
    if (kill(pid, sig) < 0)
        printf("kill: cannot signal %d\n", pid);
    else
        printf("kill: signal %d sent to %d\n", sig, pid);
}

static void do_clear(void)
{
    printf("\033[2J\033[H");
}

/* Split `cmd` at the first unquoted top-level `|` into left/right. Returns 1
   if a split happened, 0 if none. Handles single/double quotes; leading and
   trailing whitespace on each side is trimmed but interior spaces are kept. */
static size_t trim_range(const char *s, size_t n, char *out)
{
    size_t i = 0, j = n;
    while (i < j && (s[i] == ' ' || s[i] == '\t')) i++;
    while (j > i && (s[j-1] == ' ' || s[j-1] == '\t')) j--;
    size_t k = 0;
    for (size_t m = i; m < j; m++) out[k++] = s[m];
    out[k] = 0;
    return k;
}

static int split_pipe(const char *cmd, char *left, char *right)
{
    int in_s = 0, in_d = 0;
    for (const char *p = cmd; *p; p++)
    {
        if (*p == '\'' && !in_d) in_s = !in_s;
        else if (*p == '"' && !in_s) in_d = !in_d;
        if (*p == '|' && !in_s && !in_d)
        {
            size_t li = trim_range(cmd, (size_t)(p - cmd), left);
            const char *r = p + 1;
            const char *rend = r;
            for (; *rend && !(*rend == '|' && !in_s && !in_d); rend++)
            {
                if (*rend == '\'' && !in_d) in_s = !in_s;
                else if (*rend == '"' && !in_s) in_d = !in_d;
            }
            size_t ri = trim_range(r, (size_t)(rend - r), right);
            return (li && ri) ? 1 : 0;
        }
    }
    return 0;
}

/* Run "left | right" through an OS pipe (issue #31). Each side runs as a
   concurrently scheduled child spawned through sys_spawn_cmd (which inherits
   the shell's fds), wiring left stdout -> pipe write and right stdin <- pipe
   read while the shell temporarily reuses its own fd 0/1. The shell does not
   fork (kernel fork+pipe faults); it saves/restores its own std fds around
   each spawn. Left may block on the full pipe until right begins draining,
   and gets EOF when the last writer (left's child) exits. */
static int run_pipe(char *left, char *right)
{
    int fds[2];
    if (pipe(fds) < 0)
    {
        printf("sh: pipe failed\n");
        return -1;
    }

    /* --- left: stdout -> pipe write --- */
    int sav1 = dup2(1, 9);
    if (sav1 < 0) sav1 = 1;
    dup2(fds[1], 1);
    close(fds[1]);
    long p1 = sys_spawn_cmd(left, (size_t)strlen(left));
    dup2(sav1, 1);
    if (sav1 != 1) close(sav1);

    /* --- right: stdin <- pipe read --- */
    int sav0 = dup2(0, 8);
    if (sav0 < 0) sav0 = 0;
    dup2(fds[0], 0);
    close(fds[0]);
    long p2 = sys_spawn_cmd(right, (size_t)strlen(right));
    dup2(sav0, 0);
    if (sav0 != 0) close(sav0);

    if (p1 > 0) sys_waitpid((int)p1, 0);
    if (p2 > 0) sys_waitpid((int)p2, 0);
    if (p1 < 0) printf("sh: %s: command not found\n", left);
    if (p2 < 0) printf("sh: %s: command not found\n", right);
    return 0;
}

static int run_command(char *cmd)
{
    /* Expand $NAME references up front (issue #31: shell variables). */
    char expanded[LINE_MAX * 2];
    expand_vars(cmd, expanded, sizeof(expanded));
    cmd = expanded;
    size_t cmdlen = strlen(cmd);
    if (cmdlen == 0) return 0;

    /* Split on an unquoted top-level `|` into "left | right". If present, run
       both through an OS pipe (issue #31: shell pipelines). */
    char left[LINE_MAX], right[LINE_MAX];
    int split = split_pipe(cmd, left, right);
    if (split > 0)
        return run_pipe(left, right);

    int bg = parse_background(cmd);

    char copy[LINE_MAX];
    strcpy(copy, cmd);
    char *argv[MAX_ARGS];
    int argc = tokenize(copy, argv);
    if (argc == 0) return 0;

    /* If the line contains a redirection, capture the shell's own stdout so
       it can be restored afterwards (issue #31). External commands inherit the
       shell's fd 1, and builtins write to it directly, so the redirect must be
       undone once the command has run or the prompt would keep writing to the
       file. fd 9 is unused by this single-user shell. */
    int saved_stdout = -1;
    int has_redir = 0;
    for (int i = 0; i < argc; i++)
        if (argv[i][0] == '>' || argv[i][0] == '<') { has_redir = 1; break; }
    if (has_redir)
        saved_stdout = dup2(1, 9);

    /* Apply redirections uniformly (rewrites argv dropping the redirect
       tokens and dup2's the targets onto fd 0/1). Builtins then benefit from
       `>`/`<` as well as external commands. */
    if (has_redir) {
        int rc = do_redirect(argv, argc);
        if (rc < 0) { goto done; }
        argc = rc;
    }

    if (strcmp(argv[0], "exit") == 0) {
        sys_exit(0);
    } else if (strcmp(argv[0], "help") == 0) {
        print_help();
    } else if (strcmp(argv[0], "ps") == 0) {
        do_ps();
    } else if (strcmp(argv[0], "echo") == 0) {
        sh_echo(argc, argv);
    } else if (strcmp(argv[0], "cd") == 0) {
        do_cd(argc > 1 ? argv[1] : "/");
    } else if (strcmp(argv[0], "pwd") == 0) {
        do_pwd();
    } else if (strcmp(argv[0], "kill") == 0) {
        do_kill(argc, argv);
    } else if (strcmp(argv[0], "clear") == 0) {
        do_clear();
    } else if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            for (int i = 0; i < var_count; i++)
                printf("%s=%s\n", var_names[i], var_vals[i]);
        } else {
            char *eq = NULL;
            for (char *c = argv[1]; *c; c++)
                if (*c == '=') { eq = c; break; }
            if (!eq) {
                printf("export: usage: export NAME=value\n");
            } else {
                *eq = 0;
                const char *name = argv[1];
                const char *val = eq + 1;
                int found = -1;
                for (int i = 0; i < var_count; i++)
                    if (strcmp(var_names[i], name) == 0) { found = i; break; }
                if (found < 0) {
                    if (var_count >= MAX_VARS) {
                        printf("export: too many variables\n");
                    } else {
                        strncpy(var_names[var_count], name, 31);
                        var_names[var_count][31] = 0;
                        strncpy(var_vals[var_count], val, LINE_MAX - 1);
                        var_vals[var_count][LINE_MAX - 1] = 0;
                        var_count++;
                    }
                } else {
                    strncpy(var_vals[found], val, LINE_MAX - 1);
                    var_vals[found][LINE_MAX - 1] = 0;
                }
            }
        }
    } else {
        /* External command. */
        /* Rebuild a command line (space-separated) for sys_spawn_cmd, which
           does its own tokenization in the kernel. */
        char rebuild[LINE_MAX];
        char *d = rebuild;
        *d = 0;
        for (int i = 0; i < argc; i++)
        {
            if (i > 0) { *d++ = ' '; *d = 0; }
            size_t l = strlen(argv[i]);
            if (d + l >= rebuild + LINE_MAX) break;
            strcpy(d, argv[i]);
            d += l;
        }

        long pid = sys_spawn_cmd(rebuild, (size_t)(d - rebuild));
        if (pid < 0)
            printf("sh: %s: command not found\n", argv[0]);
        else if (!bg) {
            int status = 0;
            sys_waitpid((int)pid, &status);
        }
    }
done:
    /* Restore the shell's stdout so the next prompt prints to the terminal. */
    if (saved_stdout >= 0) {
        dup2(saved_stdout, 1);
        close(saved_stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    puts("");
    puts("axiome shell v1.0 (type 'help')");
    puts("");

    char line_buf[LINE_MAX];
    for (;;) {
        printf("$ ");
        read_line(line_buf, LINE_MAX);
        run_command(line_buf);
    }
    sys_exit(0);
    return 0;
}
