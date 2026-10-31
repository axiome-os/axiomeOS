#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "util.h"

static int opt_number = 0;        /* -n: number all lines */
static int opt_nonblank = 0;      /* -b: number only nonempty lines */
static int opt_squeeze = 0;       /* -s: squeeze repeated blank lines */
static int opt_showends = 0;      /* -E: display $ at end of lines */

static void usage(void)
{
    puts("Usage: cat [OPTION]... [FILE]...");
    puts("Concatenate FILE(s) to standard output, or standard input if no FILE.");
    puts("");
    puts("  -n, --number           number all output lines");
    puts("  -b, --number-nonblank  number nonempty output lines");
    puts("  -s, --squeeze-blank    suppress repeated empty output lines");
    puts("  -E, --show-ends        display $ at end of each line");
    puts("  -u                     (ignored)");
    puts("  -                      read from standard input");
    puts("      --help             display this help and exit");
}

/* Stream a file descriptor through the line-numbering / squeezing logic. */
static void cat_fd(int fd)
{
    unsigned long lineno = 0;
    int at_start = 1;
    unsigned long blank_run = 0;
    unsigned char b;
    for (;;)
    {
        long r = read(fd, &b, 1);
        if (r <= 0)
            break;
        if (at_start)
        {
            if (b == '\n')
            {
                blank_run++;
                if (opt_squeeze && blank_run > 1)
                    continue;
                if (opt_number && !opt_nonblank)
                    printf("%6lu\t", ++lineno);
                if (opt_showends)
                    putchar('$');
                putchar('\n');
                continue;
            }
            blank_run = 0;
            if (opt_number || opt_nonblank)
                printf("%6lu\t", ++lineno);
            at_start = 0;
        }
        if (b == '\n')
        {
            if (opt_showends)
                putchar('$');
            putchar('\n');
            at_start = 1;
        }
        else
        {
            putchar(b);
        }
    }
}

int main(int argc, char **argv)
{
    const char *files[256];
    int nfiles = 0;
    int end_opts = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!end_opts && a[0] == '-' && a[1] == '-' && a[2] == 0)
        {
            end_opts = 1;
            continue;
        }
        if (!end_opts && a[0] == '-' && a[1] == '-' && a[2] != 0)
        {
            const char *opt = a + 2;
            if (strcmp(opt, "number") == 0) { opt_number = 1; continue; }
            if (strcmp(opt, "number-nonblank") == 0)
            { opt_number = 1; opt_nonblank = 1; continue; }
            if (strcmp(opt, "squeeze-blank") == 0) { opt_squeeze = 1; continue; }
            if (strcmp(opt, "show-ends") == 0) { opt_showends = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            fprintf(stderr, "cat: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        /* A lone "-" is standard input, not a flag. */
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'n' && *p != 'b' && *p != 's' && *p != 'E' &&
                    *p != 'u')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "cat: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'cat --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'n') opt_number = 1;
                else if (*p == 'b') { opt_number = 1; opt_nonblank = 1; }
                else if (*p == 's') opt_squeeze = 1;
                else if (*p == 'E') opt_showends = 1;
                /* 'u' is accepted and ignored, as in GNU cat. */
            }
            continue;
        }
        if (nfiles < (int)(sizeof(files) / sizeof(files[0])))
            files[nfiles++] = a;
    }

    if (nfiles == 0)
    {
        cat_fd(0);
        sys_exit(0);
        return 0;
    }

    int st = 0;
    for (int k = 0; k < nfiles; k++)
    {
        const char *fn = files[k];
        if (strcmp(fn, "-") == 0)
        {
            cat_fd(0);
            continue;
        }
        struct stat sb;
        if (stat(fn, &sb) < 0)
        {
            fprintf(stderr, "cat: %s: %s\n", fn, ax_strerror(errno));
            st = 1;
            continue;
        }
        if (sb.st_mode & S_IFDIR)
        {
            fprintf(stderr, "cat: %s: Is a directory\n", fn);
            st = 1;
            continue;
        }
        int fd = open(fn, O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "cat: %s: %s\n", fn, ax_strerror(errno));
            st = 1;
            continue;
        }
        cat_fd(fd);
        close(fd);
    }
    sys_exit(st);
    return st;
}