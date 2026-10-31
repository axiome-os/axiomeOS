#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "signal.h"
#include "util.h"

struct sigent {
    int num;
    const char *name;
};

static const struct sigent sigs[] = {
    {SIGHUP,  "HUP"},  {SIGINT,  "INT"},  {SIGQUIT, "QUIT"},
    {SIGILL,  "ILL"},  {SIGTRAP, "TRAP"}, {SIGABRT, "ABRT"},
    {SIGBUS,  "BUS"},  {SIGFPE,  "FPE"},  {SIGKILL, "KILL"},
    {SIGUSR1, "USR1"}, {SIGSEGV, "SEGV"}, {SIGUSR2, "USR2"},
    {SIGPIPE, "PIPE"}, {SIGALRM, "ALRM"}, {SIGTERM, "TERM"},
    {SIGCHLD, "CHLD"}, {SIGCONT, "CONT"}, {SIGSTOP, "STOP"}
};
#define NSIGENTS ((int)(sizeof(sigs) / sizeof(sigs[0])))

static int name_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static const struct sigent *find_name(const char *s)
{
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (int i = 0; i < NSIGENTS; i++)
        if (name_eq(sigs[i].name, s))
            return &sigs[i];
    return 0;
}

static const struct sigent *find_num(int n)
{
    for (int i = 0; i < NSIGENTS; i++)
        if (sigs[i].num == n)
            return &sigs[i];
    return 0;
}

static void usage(void)
{
    puts("Usage: kill [-s SIGNAL | -SIGNAL | -l [SIGNAL...]] PID...");
    puts("Send SIGNAL to the PID(s); default signal is TERM.");
    puts("");
    puts("  -s, --signal=SIGNAL  signal to send");
    puts("  -l, --list[=LIST]    list signal names, or convert SIGNAL");
    puts("  -SIGNAL              send SIGNAL (name or number)");
    puts("      --help           display this help and exit");
}

static int parse_sig(const char *s, int *out)
{
    if (s[0] >= '0' && s[0] <= '9')
    {
        int n = atoi(s);
        if (n <= 0 || n >= NSIG)
            return -1;
        *out = n;
        return 0;
    }
    const struct sigent *e = find_name(s);
    if (!e)
        return -1;
    *out = e->num;
    return 0;
}

static int all_digits(const char *s)
{
    if (!*s)
        return 0;
    while (*s)
        if (*s < '0' || *s > '9')
            return 0;
        else
            s++;
    return 1;
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int list = 0;
    int pids[256];
    int npids = 0;
    int end_opts = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!end_opts && strcmp(a, "--") == 0)
        {
            end_opts = 1;
            continue;
        }
        if (end_opts)
        {
            if (!all_digits(a))
            {
                fprintf(stderr, "kill: invalid process id '%s'\n", a);
                sys_exit(1);
            }
            if (npids < (int)(sizeof(pids) / sizeof(pids[0])))
                pids[npids++] = atoi(a);
            continue;
        }
        if (strcmp(a, "--help") == 0) { usage(); sys_exit(0); }
        if (strcmp(a, "--version") == 0)
        {
            puts("kill (axiomeOS) 1.0");
            sys_exit(0);
        }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0)
        {
            list = 1;
            continue;
        }
        if (strncmp(a, "--list=", 7) == 0)
        {
            list = 2;
            if (parse_sig(a + 7, &sig) < 0)
            {
                fprintf(stderr, "kill: invalid signal '%s'\n", a + 7);
                sys_exit(1);
            }
            continue;
        }
        if (strcmp(a, "-s") == 0 || strcmp(a, "--signal") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "kill: option requires an argument -- 's'\n");
                sys_exit(1);
            }
            if (parse_sig(argv[++i], &sig) < 0)
            {
                fprintf(stderr, "kill: invalid signal '%s'\n", argv[i]);
                sys_exit(1);
            }
            continue;
        }
        if (strncmp(a, "--signal=", 9) == 0)
        {
            if (parse_sig(a + 9, &sig) < 0)
            {
                fprintf(stderr, "kill: invalid signal '%s'\n", a + 9);
                sys_exit(1);
            }
            continue;
        }
        if (a[0] == '-' && a[1] != 0 && a[1] != '-')
        {
            /* -<signal> */
            const struct sigent *e = find_name(a + 1);
            if (a[1] >= '0' && a[1] <= '9')
            {
                int n = atoi(a + 1);
                if (n <= 0 || n >= NSIG)
                {
                    fprintf(stderr, "kill: invalid signal number '%s'\n", a + 1);
                    sys_exit(1);
                }
                sig = n;
            }
            else if (e)
            {
                sig = e->num;
            }
            else
            {
                fprintf(stderr, "kill: invalid option -- '%c'\n", a[1]);
                sys_exit(1);
            }
            continue;
        }
        if (a[0] == '-' && a[1] == 0)
        {
            fprintf(stderr, "kill: unrecognized option '-'\n");
            sys_exit(1);
        }
        if (!all_digits(a))
        {
            fprintf(stderr, "kill: invalid process id '%s'\n", a);
            sys_exit(1);
        }
        if (npids < (int)(sizeof(pids) / sizeof(pids[0])))
            pids[npids++] = atoi(a);
    }

    if (list)
    {
        if (npids == 0)
        {
            for (int i = 0; i < NSIGENTS; i++)
            {
                if (i > 0)
                    putchar(' ');
                printf("%s", sigs[i].name);
            }
            putchar('\n');
            sys_exit(0);
        }
        for (int i = 0; i < npids; i++)
        {
            const struct sigent *e = find_num(pids[i]);
            if (e)
                printf("%s\n", e->name);
            else
                fprintf(stderr, "kill: unknown signal %d\n", pids[i]);
        }
        sys_exit(0);
    }

    if (npids == 0)
    {
        fprintf(stderr, "kill: missing operand\n");
        fprintf(stderr, "Try 'kill --help' for more information.\n");
        sys_exit(1);
    }

    int st = 0;
    for (int i = 0; i < npids; i++)
    {
        if (kill(pids[i], sig) < 0)
        {
            fprintf(stderr, "kill: %d: %s\n", pids[i], ax_strerror(errno));
            st = 1;
        }
    }
    sys_exit(st);
    return st;
}