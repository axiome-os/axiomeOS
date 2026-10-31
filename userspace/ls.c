#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "util.h"

#define MAX_E 1024

struct opts {
    int all;        /* -a: list everything */
    int almost;     /* -A: list everything except . and .. */
    int longfmt;    /* -l */
    int recursive;  /* -R */
    int reverse;    /* -r */
    int help;
};

static void usage(void)
{
    puts("Usage: ls [OPTION]... [FILE]...");
    puts("List information about the FILEs (the current directory by default).");
    puts("");
    puts("  -a, --all          do not ignore entries starting with .");
    puts("  -A, --almost-all   do not list implied . and ..");
    puts("  -l                 use a long listing format");
    puts("  -r, --reverse      reverse order while sorting");
    puts("  -R, --recursive    list subdirectories recursively");
    puts("      --help         display this help and exit");
}

static void sort_ents(struct vfs_dirent *e, int n)
{
    for (int i = 1; i < n; i++)
    {
        int j = i;
        while (j > 0 && strcmp(e[j - 1].name, e[j].name) > 0)
        {
            struct vfs_dirent t = e[j - 1];
            e[j - 1] = e[j];
            e[j] = t;
            j--;
        }
    }
}

static int is_dot(const char *n)
{
    return (n[0] == '.' && n[1] == 0) ||
           (n[0] == '.' && n[1] == '.' && n[2] == 0);
}

static void reverse_ents(struct vfs_dirent *e, int n)
{
    for (int i = 0, j = n - 1; i < j; i++, j--)
    {
        struct vfs_dirent t = e[i];
        e[i] = e[j];
        e[j] = t;
    }
}

/* List one directory (assumed to exist). */
static int list_dir(const char *path, const struct opts *o)
{
    struct vfs_dirent all[MAX_E];
    int n = readdir(path, all, MAX_E);
    if (n < 0)
    {
        fprintf(stderr, "ls: cannot access '%s': %s\n",
                path, ax_strerror(errno));
        return 1;
    }
    if (n > MAX_E)
        n = MAX_E;

    struct vfs_dirent shown[MAX_E];
    int ns = 0;
    for (int i = 0; i < n; i++)
    {
        const char *nm = all[i].name;
        if (is_dot(nm))
        {
            if (o->all)
                shown[ns++] = all[i];
        }
        else if (nm[0] == '.')
        {
            if (o->all || o->almost)
                shown[ns++] = all[i];
        }
        else
        {
            shown[ns++] = all[i];
        }
    }

    sort_ents(shown, ns);
    if (o->reverse)
        reverse_ents(shown, ns);

    int st = 0;
    for (int i = 0; i < ns; i++)
    {
        const char *nm = shown[i].name;
        if (o->longfmt)
        {
            char full[512];
            ax_join_path(path, nm, full, sizeof(full));
            struct stat sb;
            if (stat(full, &sb) < 0)
            {
                fprintf(stderr, "ls: cannot access '%s': %s\n",
                        full, ax_strerror(errno));
                st = 1;
                continue;
            }
            char m[11];
            ax_mode_str(sb.st_mode, m);
            printf("%s %3d %8llu %s\n", m, sb.st_nlink,
                   (unsigned long long)sb.st_size, nm);
        }
        else
        {
            printf("%s\n", nm);
        }
    }

    if (o->recursive)
    {
        for (int i = 0; i < ns; i++)
        {
            if (shown[i].type != DT_DIR || is_dot(shown[i].name))
                continue;
            char full[512];
            ax_join_path(path, shown[i].name, full, sizeof(full));
            printf("\n%s:\n", full);
            if (list_dir(full, o) < 0)
                st = 1;
        }
    }
    return st;
}

/* Show one operand (a file or a directory). */
static int show_one(const char *path, const struct opts *o)
{
    struct stat sb;
    if (stat(path, &sb) < 0)
    {
        fprintf(stderr, "ls: cannot access '%s': %s\n",
                path, ax_strerror(errno));
        return 1;
    }
    if (!(sb.st_mode & S_IFDIR))
    {
        if (o->longfmt)
        {
            char m[11];
            ax_mode_str(sb.st_mode, m);
            printf("%s %3d %8llu %s\n", m, sb.st_nlink,
                   (unsigned long long)sb.st_size, path);
        }
        else
        {
            printf("%s\n", path);
        }
        return 0;
    }
    return list_dir(path, o);
}

int main(int argc, char **argv)
{
    struct opts o;
    memset(&o, 0, sizeof(o));
    const char *ops[256];
    int nops = 0;
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
            if (strcmp(opt, "all") == 0) { o.all = 1; continue; }
            if (strcmp(opt, "almost-all") == 0) { o.almost = 1; continue; }
            if (strcmp(opt, "reverse") == 0) { o.reverse = 1; continue; }
            if (strcmp(opt, "recursive") == 0) { o.recursive = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            if (strcmp(opt, "long-format") == 0) { o.longfmt = 1; continue; }
            fprintf(stderr, "ls: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'a' && *p != 'A' && *p != 'l' && *p != 'r' &&
                    *p != 'R')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "ls: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'ls --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'a') o.all = 1;
                else if (*p == 'A') o.almost = 1;
                else if (*p == 'l') o.longfmt = 1;
                else if (*p == 'r') o.reverse = 1;
                else if (*p == 'R') o.recursive = 1;
            }
            continue;
        }
        if (nops < (int)(sizeof(ops) / sizeof(ops[0])))
            ops[nops++] = a;
    }

    int st = 0;
    if (nops == 0)
    {
        ops[nops++] = ".";
    }

    /* A directory operand resolves to the directory itself. */
    int isdir[256];
    int ndirs = 0;
    for (int i = 0; i < nops; i++)
    {
        struct stat sb;
        isdir[i] = (stat(ops[i], &sb) == 0 && (sb.st_mode & S_IFDIR));
        if (isdir[i])
            ndirs++;
    }

    /* File operands first, as plain listings (no headers). */
    for (int i = 0; i < nops; i++)
        if (!isdir[i])
            if (show_one(ops[i], &o) < 0)
                st = 1;

    /* Then directory operands, with headers when more than one. */
    int hdrs = 0;
    for (int i = 0; i < nops; i++)
    {
        if (!isdir[i])
            continue;
        if (ndirs > 1)
        {
            if (hdrs)
                printf("\n");
            printf("%s:\n", ops[i]);
        }
        if (list_dir(ops[i], &o) < 0)
            st = 1;
        hdrs++;
    }
    sys_exit(st);
    return st;
}