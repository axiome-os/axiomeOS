#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"

/* ===========================================================================
 * axiome-init - the system init (PID 1) and service manager.
 *
 * Responsibilities (per the project spec):
 *   * start services defined in /etc/axiome-init.conf
 *   * honour dependencies between services (start order)
 *   * supervise them: restart on crash, with a crash-loop rate limit
 *   * log service lifecycle events
 *
 * Design notes (this is a minimal, single-threaded OS):
 *   * Children are spawned with sys_spawn_cmd(); descriptors and cwd now
 *     inherit like fork/exec, but axiome-init still keeps service logging
 *     explicit so lifecycle messages stay separate from service output.
 *   * Reaping uses the kernel's blocking sys_waitpid(-1). When a child exits
 *     the scheduler wakes init, so this is an efficient idle/supervise loop.
 *   * PID 1 is auto-respawned by the kernel if it exits, so axiome-init must
 *     never exit on its own. If there is nothing to supervise it drops to a
 *     rescue shell instead.
 * =========================================================================== */

#define MAX_UNITS   32
#define NAME_LEN    32
#define CMD_LEN     256
#define DEP_LEN     128
#define LOG_LINE    320

#define INIT_LOG    "/var/log/axiome-init.log"
#define INIT_CONF   "/etc/axiome-init.conf"

/* Restart policy for a unit. */
enum restart_policy {
    RESTART_ONCE = 0,       /* run once; do not restart on exit */
    RESTART_ALWAYS,         /* restart regardless of exit status */
    RESTART_ON_FAILURE      /* restart only on non-zero / signalled exit */
};

/* Lifecycle state of a unit. */
enum unit_state {
    UNIT_DOWN = 0,          /* not yet started */
    UNIT_RUNNING,           /* spawned and alive */
    UNIT_EXITED,            /* ran once, exited cleanly (restart=once) */
    UNIT_FAILED,            /* gave up (crash loop / failed to spawn) */
    UNIT_DISABLED          /* a dependency failed; cannot start */
};

struct unit {
    char name[NAME_LEN];
    char command[CMD_LEN];
    char depends[DEP_LEN];      /* raw comma/space separated dependency names */
    enum restart_policy policy;
    int  maxfail;               /* 0 = unlimited restarts */
    long pid;                   /* current child pid, or -1 */
    enum unit_state state;
    int  restarts;              /* total (re)start attempts */
    int  fails;                 /* consecutive failures */
};

static struct unit g_units[MAX_UNITS];
static int g_nunits = 0;

static int g_logfd = -1;

/* ---- tiny string helpers (libc has no strdup/strtok) -------------------- */

static void my_strcpy(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = 0;
}

/* Trim leading/trailing spaces and tabs in place; returns ptr to start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* ------------------------------------------------------------------ logging */

static void log_raw(const char *buf, size_t len)
{
    /* Log to the file only. fd 1 is CONSOLE_OUT, which the kernel mirrors to
       the framebuffer as well as serial; writing lifecycle spam there is what
       put axiome-init messages on the framebuffer. Fall back to the console
       only if the log file could not be opened. */
    if (g_logfd >= 0)
        write(g_logfd, buf, len);
    else
        write(1, buf, len);
}

/* Format: "[axiome-init] <seq> <svc>: <evt>\n" and emit it. */
static void log_evt(const char *svc, const char *evt)
{
    static unsigned long seq = 0;
    char line[LOG_LINE];
    int n = 0;
#define AP(s) do { const char *_s=(s); while(*_s) line[n++]=*_s++; } while(0)
    AP("[axiome-init] ");
    /* sequence number */
    char num[16]; int ni = 0; unsigned long v = seq;
    if (v == 0) num[ni++] = '0';
    while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni--) line[n++] = num[ni];
    AP(" ");
    AP(svc); AP(": ");
    AP(evt); AP("\n");
#undef AP
    seq++;
    log_raw(line, (size_t)n);
}

/* ---------------------------------------------------------- config parsing */

static struct unit *unit_by_name(const char *name)
{
    for (int i = 0; i < g_nunits; i++)
        if (strcmp(g_units[i].name, name) == 0)
            return &g_units[i];
    return 0;
}

static int parse_policy(const char *s)
{
    if (strcmp(s, "always") == 0)     return RESTART_ALWAYS;
    if (strcmp(s, "on-failure") == 0) return RESTART_ON_FAILURE;
    return RESTART_ONCE;
}

/* Read an entire file into a malloc'd, NUL-terminated buffer (or 0). */
static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0)
    {
        close(fd);
        return 0;
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf)
    {
        close(fd);
        return 0;
    }
    size_t got = 0;
    while (got < (size_t)st.st_size)
    {
        long r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    buf[got] = 0;
    close(fd);
    return buf;
}

/* Parse the INI-like config into g_units. Returns number of units. */
static int load_config(const char *path)
{
    char *text = read_file(path);
    if (!text)
        return 0;

    struct unit *cur = 0;
    char *line = text;
    while (*line)
    {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int had_nl = (*nl == '\n');
        *nl = 0;

        char *s = trim(line);
        if (*s && *s != '#')
        {
            if (s[0] == '[' && s[strlen(s) - 1] == ']')
            {
                /* new unit header */
                s[strlen(s) - 1] = 0;
                char *nm = trim(s + 1);
                if (g_nunits < MAX_UNITS)
                {
                    cur = &g_units[g_nunits];
                    memset(cur, 0, sizeof(*cur));
                    cur->pid = -1;
                    cur->state = UNIT_DOWN;
                    cur->policy = RESTART_ONCE;
                    my_strcpy(cur->name, nm);
                    g_nunits++;
                }
                else
                {
                    log_evt("config", "too many units; ignoring further entries");
                    cur = 0;
                }
            }
            else if (cur)
            {
                char *eq = s;
                while (*eq && *eq != '=') eq++;
                if (*eq)
                {
                    *eq = 0;
                    char *key = trim(s);
                    char *val = trim(eq + 1);
                    if (strcmp(key, "command") == 0)
                        my_strcpy(cur->command, val);
                    else if (strcmp(key, "depends") == 0)
                        my_strcpy(cur->depends, val);
                    else if (strcmp(key, "restart") == 0)
                        cur->policy = parse_policy(val);
                    else if (strcmp(key, "maxfail") == 0)
                        cur->maxfail = atoi(val);
                    /* unknown keys (e.g. "log") are ignored as metadata */
                }
            }
        }

        line = nl + (had_nl ? 1 : 0);
    }
    free(text);
    return g_nunits;
}

/* ------------------------------------------------------ dependency helpers */

/* Returns 1 if all of u's dependencies are satisfied (RUNNING or EXITED),
   0 if a dependency is still starting (blocked), or -1 if a dependency is
   missing / failed / disabled (unsatisfiable). */
static int deps_satisfied(struct unit *u)
{
    if (u->depends[0] == 0)
        return 1;
    char buf[DEP_LEN];
    my_strcpy(buf, u->depends);
    char *tok = buf;
    int block = 0;
    while (*tok)
    {
        while (*tok == ' ' || *tok == ',' || *tok == '\t') tok++;
        if (!*tok) break;
        char *end = tok;
        while (*end && *end != ' ' && *end != ',' && *end != '\t') end++;
        char saved = *end; *end = 0;
        struct unit *d = unit_by_name(tok);
        *end = saved;
        tok = end;
        if (!d)
            return -1;                       /* dependency does not exist */
        if (d->state == UNIT_RUNNING || d->state == UNIT_EXITED)
            continue;
        if (d->state == UNIT_FAILED || d->state == UNIT_DISABLED)
            return -1;                       /* unsatisfiable */
        block = 1;                           /* still starting */
    }
    return block ? 0 : 1;
}

static int any_active(void)
{
    for (int i = 0; i < g_nunits; i++)
        if (g_units[i].state == UNIT_RUNNING)
            return 1;
    return 0;
}

/* Start a unit's process. Returns 0 on success. */
static int start_unit(struct unit *u)
{
    u->restarts++;
    long pid = sys_spawn_cmd(u->command, strlen(u->command));
    if (pid < 0)
    {
        log_evt(u->name, "failed to spawn (command not found?)");
        u->state = UNIT_FAILED;
        return -1;
    }
    u->pid = pid;
    u->state = UNIT_RUNNING;
    char msg[64];
    int n = 0;
    const char *pre = "started pid ";
    for (const char *p = pre; *p; p++) msg[n++] = *p;
    char num[16]; int ni = 0; long v = pid;
    if (v == 0) num[ni++] = '0';
    while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni--) msg[n++] = num[ni];
    msg[n] = 0;
    log_evt(u->name, msg);
    return 0;
}

/* Begin all units whose dependencies are ready. Repeats until no progress,
   which naturally resolves ordering and detects missing/cyclic deps. */
static void start_all(void)
{
    int progress = 1;
    while (progress)
    {
        progress = 0;
        for (int i = 0; i < g_nunits; i++)
        {
            struct unit *u = &g_units[i];
            if (u->state != UNIT_DOWN)
                continue;
            int r = deps_satisfied(u);
            if (r > 0)
            {
                start_unit(u);
                progress = 1;
            }
            else if (r < 0)
            {
                u->state = UNIT_DISABLED;
                log_evt(u->name, "disabled: dependency unsatisfied");
                progress = 1;
            }
            /* r == 0: still waiting on a starting dependency */
        }
    }
    /* Anything still UNIT_DOWN could not be started (e.g. a dependency
       cycle). Mark it disabled. */
    for (int i = 0; i < g_nunits; i++)
        if (g_units[i].state == UNIT_DOWN)
        {
            g_units[i].state = UNIT_DISABLED;
            log_evt(g_units[i].name, "disabled: unsatisfiable (cycle?)");
        }
}

/* -------------------------------------------------------- supervision loop */

/* Bounded backoff between restarts: sleep 100ms * restarts, capped at 4s. */
static void backoff(int restarts)
{
    long ms = restarts * 100;
    if (ms > 4000) ms = 4000;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

/* Handle a child that exited: decide whether to restart, applying the policy
   and crash-loop rate limit. */
static void on_child_exit(struct unit *u, int status)
{
    int signalled = (status >= 128);   /* kernel encodes signal death as 128+sig */
    int failed = signalled || status != 0;

    char msg[96];
    int n = 0;
    const char *pre = failed ? "exited uncleanly (status " : "exited cleanly (status ";
    for (const char *p = pre; *p; p++) msg[n++] = *p;
    char num[16]; int ni = 0; int v = status;
    if (v == 0) num[ni++] = '0';
    while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni--) msg[n++] = num[ni];
    msg[n++] = ')'; msg[n] = 0;
    log_evt(u->name, msg);

    int want_restart = 0;
    if (u->policy == RESTART_ALWAYS)
        want_restart = 1;
    else if (u->policy == RESTART_ON_FAILURE)
        want_restart = failed;
    else
        want_restart = 0;

    if (!want_restart)
    {
        u->state = failed ? UNIT_FAILED : UNIT_EXITED;
        u->pid = -1;
        if (!failed)
            log_evt(u->name, "stopped (no restart requested)");
        return;
    }

    if (failed)
        u->fails++;
    else
        u->fails = 0;

    if (u->maxfail > 0 && u->restarts >= u->maxfail)
    {
        u->state = UNIT_FAILED;
        u->pid = -1;
        log_evt(u->name, "crash loop: max restarts exceeded; giving up");
        return;
    }

    backoff(u->restarts);
    long pid = sys_spawn_cmd(u->command, strlen(u->command));
    if (pid < 0)
    {
        u->state = UNIT_FAILED;
        u->pid = -1;
        log_evt(u->name, "respawn failed; giving up");
        return;
    }
    u->pid = pid;
    u->state = UNIT_RUNNING;

    char rmsg[64];
    int rn = 0;
    const char *rp = "restarted (attempt #";
    for (const char *p = rp; *p; p++) rmsg[rn++] = *p;
    char rnum[16]; int rni = 0; int rv = u->restarts + 1;
    if (rv == 0) rnum[rni++] = '0';
    while (rv) { rnum[rni++] = (char)('0' + (rv % 10)); rv /= 10; }
    while (rni--) rmsg[rn++] = rnum[rni];
    rmsg[rn++] = ')'; rmsg[rn] = 0;
    log_evt(u->name, rmsg);
}

static struct unit *find_unit_by_pid(long pid)
{
    for (int i = 0; i < g_nunits; i++)
        if (g_units[i].pid == pid)
            return &g_units[i];
    return 0;
}

/* ------------------------------------------------------------- rescue shell */

static long g_rescue_pid = -1;

static void spawn_rescue(void)
{
    g_rescue_pid = sys_spawn_cmd("/bin/sh", strlen("/bin/sh"));
    if (g_rescue_pid < 0)
    {
        /* Last resort: nothing we can supervise and no shell. Spin idly so the
           kernel does not consider init dead, and keep trying. */
        log_evt("init", "no services and /bin/sh unavailable; idling");
        for (volatile int i = 0; i < 1000; i++) sys_yield();
    }
    else
    {
        log_evt("init", "no services to supervise; starting rescue shell");
    }
}

/* First-boot setup is owned by the desktop: guixd opens the axoobe window
   when /etc/passwd has no regular user yet. The CLI /bin/oobe stays
   available for manual/headless use but is deliberately NOT run here, so a
   fresh boot goes straight to the graphical setup instead of blocking on
   the serial console. */

/* Load every .kxt under /System/Extensions (loadable kernel modules such as
   the e1000 NIC driver). Best-effort: failures are logged but non-fatal. */
static void load_modules(void)
{
    struct vfs_dirent ents[64];
    int n = readdir("/System/Extensions", ents, 64);
    if (n <= 0)
    {
        log_evt("init", "no modules to load");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        if (ents[i].type == DT_DIR)
            continue;
        const char *nm = ents[i].name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 4, ".kxt") != 0)
            continue;
        char path[256];
        int k = 0;
        const char *pre = "/System/Extensions/";
        for (; *pre; pre++) path[k++] = *pre;
        for (const char *p = nm; *p; p++) path[k++] = *p;
        path[k] = 0;
        if (kxtload(path) == 0)
            log_evt("init", "loaded module");
        else
            log_evt("init", "module load failed");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* NOTE: the runtime O_CREAT (file creation) path in axiomefs is currently
        broken, so the log file is created at image-build time (see
        root_manifest.txt) and opened here for append only. */
    g_logfd = open(INIT_LOG, O_WRONLY | O_APPEND);
    if (g_logfd < 0)
        log_evt("init", "could not open log file; console only");

    log_evt("init", "axiome-init starting (pid 1)");

    load_modules();

    int n = load_config(INIT_CONF);
    if (n == 0)
    {
        log_evt("init", "no services configured; entering rescue shell");
        spawn_rescue();
    }
    else
    {
        char msg[64];
        int k = 0;
        const char *p = "loaded ";
        for (; *p; p++) msg[k++] = *p;
        char nn[8]; int mi = 0; int v = n;
        if (v == 0) nn[mi++] = '0';
        while (v) { nn[mi++] = (char)('0' + (v % 10)); v /= 10; }
        while (mi--) msg[k++] = nn[mi];
        const char *s2 = " service(s)";
        for (const char *q = s2; *q; q++) msg[k++] = *q;
        msg[k] = 0;
        log_evt("init", msg);
        start_all();
    }

    /* Supervise. Block in waitpid; the scheduler wakes us when a child dies.
       We never exit (PID 1 auto-respawns), so a dead system falls back to a
       rescue shell instead. */
    for (;;)
    {
        int status = 0;
        long pid = sys_waitpid(-1, &status);
        if (pid < 0)
        {
            /* No children to reap. If a service is still running this is
               transient; otherwise drop to the rescue shell. */
            if (!any_active() && g_rescue_pid < 0)
                spawn_rescue();
            continue;
        }

        if (pid == g_rescue_pid)
        {
            /* Rescue shell exited; respawn so the console stays usable. */
            g_rescue_pid = -1;
            continue;
        }

        struct unit *u = find_unit_by_pid(pid);
        if (!u)
            continue;   /* unknown child; ignore */

        u->pid = -1;
        on_child_exit(u, status);
    }

    /* unreachable */
    sys_exit(0);
    return 0;
}
