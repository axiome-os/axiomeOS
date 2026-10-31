#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "util.h"

static int opt_nocreate = 0;   /* -c / --no-create */

static void usage(void)
{
    puts("Usage: touch [OPTION]... FILE...");
    puts("Update the access and modification times of each FILE to the"
         " current time.");
    puts("If the FILE does not exist, it is created empty.");
    puts("");
    puts("  -a, --time=atime     change only the access time (no-op here)");
    puts("  -c, --no-create      do not create any files");
    puts("  -m, --time=mtime     change only the modification time"
         " (no-op here)");
    puts("      --help           display this help and exit");
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
            if (strcmp(opt, "no-create") == 0) { opt_nocreate = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            if (strncmp(opt, "time=", 5) == 0) { continue; } /* -a/-m accepted */
            if (strcmp(opt, "atime") == 0) { continue; }
            fprintf(stderr, "touch: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'a' && *p != 'c' && *p != 'm')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "touch: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'touch --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
                if (*p == 'c')
                    opt_nocreate = 1;
            continue;
        }
        if (nfiles < (int)(sizeof(files) / sizeof(files[0])))
            files[nfiles++] = a;
    }

    if (nfiles == 0)
    {
        fprintf(stderr, "touch: missing operand\n");
        fprintf(stderr, "Try 'touch --help' for more information.\n");
        sys_exit(1);
    }

    int st = 0;
    for (int i = 0; i < nfiles; i++)
    {
        const char *fn = files[i];
        struct stat sb;
        if (stat(fn, &sb) == 0)
        {
            /* Exists: no timestamp-update facility, so nothing to do. */
            continue;
        }
        if (errno != ENOENT)
        {
            fprintf(stderr, "touch: cannot touch '%s': %s\n",
                    fn, ax_strerror(errno));
            st = 1;
            continue;
        }
        if (opt_nocreate)
            continue;
        int fd = open(fn, O_CREAT | O_WRONLY);
        if (fd < 0)
        {
            fprintf(stderr, "touch: cannot touch '%s': %s\n",
                    fn, ax_strerror(errno));
            st = 1;
        }
        else
        {
            close(fd);
        }
    }
    sys_exit(st);
    return st;
}