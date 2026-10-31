#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"
#include "util.h"

static int opt_parents = 0;   /* -p */
static int opt_verbose = 0;   /* -v */
static unsigned int opt_mode = 0;
static int have_mode = 0;     /* -m MODE */

static void usage(void)
{
    puts("Usage: mkdir [OPTION]... DIRECTORY...");
    puts("Create the DIRECTORY(ies), if they do not already exist.");
    puts("");
    puts("  -m, --mode=MODE    set file mode (as in chmod), not a=rwx - umask");
    puts("  -p, --parents      make parent directories as needed");
    puts("  -v, --verbose      print a message for each created directory");
    puts("      --help         display this help and exit");
}

static unsigned int parse_octal(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '7')
        v = v * 8 + (unsigned int)(*s++ - '0');
    return v;
}

/* Check the whole string is a valid octal mode. */
static int is_octal(const char *s)
{
    if (!*s)
        return 0;
    while (*s)
        if (*s < '0' || *s > '7')
            return 0;
        else
            s++;
    return 1;
}

static void apply_mode(const char *path)
{
    if (chmod(path, opt_mode) < 0)
        fprintf(stderr, "mkdir: cannot set permissions of '%s': %s\n",
                path, ax_strerror(errno));
}

static int make_one(const char *path)
{
    if (!opt_parents)
    {
        if (mkdir(path) < 0)
        {
            fprintf(stderr, "mkdir: cannot create directory '%s': %s\n",
                    path, ax_strerror(errno));
            return -1;
        }
        if (have_mode)
            apply_mode(path);
        if (opt_verbose)
            printf("mkdir: created directory '%s'\n", path);
        return 0;
    }

    /* Create each intermediate component (including the final one). */
    char tmp[512];
    size_t len = 0;
    while (path[len] && len < sizeof(tmp) - 1) { tmp[len] = path[len]; len++; }
    tmp[len] = 0;

    for (size_t i = 1; i <= len; i++)
    {
        if (tmp[i] == '/' || i == len)
        {
            if (i == len && tmp[i] == '/')
                break;
            char save = tmp[i];
            tmp[i] = 0;
            int made = 0;
            if (mkdir(tmp) == 0)
            {
                made = 1;
            }
            else if (errno != EEXIST)
            {
                fprintf(stderr, "mkdir: cannot create directory '%s': %s\n",
                        tmp, ax_strerror(errno));
                tmp[i] = save;
                return -1;
            }
            tmp[i] = save;
            if (made && i == len)
            {
                if (have_mode)
                    apply_mode(path);
                if (opt_verbose)
                    printf("mkdir: created directory '%s'\n", path);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dirs[256];
    int ndirs = 0;
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
            if (strcmp(opt, "parents") == 0) { opt_parents = 1; continue; }
            if (strcmp(opt, "verbose") == 0) { opt_verbose = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            if (strncmp(opt, "mode=", 5) == 0)
            {
                if (!is_octal(opt + 5))
                {
                    fprintf(stderr, "mkdir: invalid mode '%s'\n", opt + 5);
                    sys_exit(1);
                }
                opt_mode = parse_octal(opt + 5);
                have_mode = 1;
                continue;
            }
            fprintf(stderr, "mkdir: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'p' && *p != 'v' && *p != 'm')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "mkdir: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'mkdir --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'p') opt_parents = 1;
                else if (*p == 'v') opt_verbose = 1;
                else if (*p == 'm')
                {
                    if (i + 1 >= argc)
                    {
                        fprintf(stderr,
                                "mkdir: option requires an argument -- 'm'\n");
                        sys_exit(1);
                    }
                    const char *md = argv[++i];
                    if (!is_octal(md))
                    {
                        fprintf(stderr, "mkdir: invalid mode '%s'\n", md);
                        sys_exit(1);
                    }
                    opt_mode = parse_octal(md);
                    have_mode = 1;
                }
            }
            continue;
        }
        if (ndirs < (int)(sizeof(dirs) / sizeof(dirs[0])))
            dirs[ndirs++] = a;
    }

    if (ndirs == 0)
    {
        fprintf(stderr, "mkdir: missing operand\n");
        fprintf(stderr, "Try 'mkdir --help' for more information.\n");
        sys_exit(1);
    }

    int st = 0;
    for (int i = 0; i < ndirs; i++)
        if (make_one(dirs[i]) < 0)
            st = 1;
    sys_exit(st);
    return st;
}