#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"

#ifndef ENOTEMPTY
#define ENOTEMPTY 39
#endif

static int g_force = 0;
static int g_verbose = 0;

static const char *rm_strerror(int e)
{
    switch (e)
    {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    default: return "Unknown error";
    }
}

static int is_root_path(const char *path)
{
    const char *p = path;
    /* Skip leading slashes; require at least one. */
    if (*p != '/')
        return 0;
    while (*p == '/')
        p++;
    if (*p == 0)
        return 1; /* all slashes: "/", "//", ... */
    /* Allow a trailing "/." / "/./" form: "/.", "/./", "//./" ... */
    if (*p == '.' && (p[1] == 0 || p[1] == '/'))
    {
        p++;
        while (*p == '/')
            p++;
        if (*p == 0)
            return 1;
        /* "/./..." with more components is not root. */
        return 0;
    }
    return 0;
}

static void join_child(const char *dir, const char *name, char *out, size_t cap)
{
    size_t pl = 0;
    while (dir[pl] && pl + 1 < cap) { out[pl] = dir[pl]; pl++; }
    if (pl > 0 && out[pl - 1] != '/' && pl + 1 < cap) out[pl++] = '/';
    size_t nl = 0;
    while (name[nl] && pl + nl + 1 < cap) { out[pl + nl] = name[nl]; nl++; }
    out[pl + nl] = 0;
}

static int rm_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
    {
        if (g_force && errno == ENOENT)
            return 0;
        printf("rm: cannot remove '%s': %s\n", path, rm_strerror(errno));
        return -1;
    }

    if (!(st.st_mode & S_IFDIR))
    {
        if (unlink(path) < 0)
        {
            if (g_force && errno == ENOENT)
                return 0;
            printf("rm: cannot remove '%s': %s\n", path, rm_strerror(errno));
            return -1;
        }
        if (g_verbose)
            printf("removed '%s'\n", path);
        return 0;
    }

    /* Directory: delete children in bounded batches, then the dir itself.
       Batching keeps the on-stack dirent buffer small (issue #31: `rm -r`
       page-faulted with a 64KB buffer) and handles directories with more
       entries than fit in one readdir call. */
    int status = 0;
    for (;;)
    {
        struct vfs_dirent ents[64];
        int n = readdir(path, ents, 64);
        if (n < 0)
        {
            if (g_force && errno == ENOENT && status == 0)
                return 0;
            printf("rm: cannot remove '%s': %s\n", path, rm_strerror(errno));
            return -1;
        }
        int real = 0;
        int removed_any = 0;
        for (int i = 0; i < n; i++)
        {
            const char *name = ents[i].name;
            if (name[0] == '.' &&
                (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            real++;
            char child[512];
            join_child(path, name, child, sizeof(child));
            if (child[0] == 0)
            {
                printf("rm: cannot remove '%s/%s': Invalid argument\n",
                       path, name);
                status = -1;
                continue;
            }
            if (rm_recursive(child) < 0)
                status = -1;
            else
                removed_any = 1;
        }
        if (real == 0)
            break;
        if (n < 64)
            break; /* got the whole directory; don't re-list */
        if (!removed_any)
            break; /* no progress: avoid looping forever on errors */
    }

    if (status < 0)
    {
        /* One or more children failed; rmdir would fail with ENOTEMPTY.
           Try it anyway so the error reflects the directory state, but
           keep going (GNU rm keeps removing siblings and reports all
           failures instead of aborting at the first one). */
    }
    if (rmdir(path) < 0)
    {
        if (g_force && errno == ENOENT && status == 0)
            return 0;
        printf("rm: cannot remove '%s': %s\n", path, rm_strerror(errno));
        return -1;
    }
    if (g_verbose)
        printf("removed directory '%s'\n", path);
    return status;
}

static void print_usage(void)
{
    puts("Usage: rm [OPTION]... [FILE]...");
    puts("Remove (unlink) the FILE(s).");
    puts("");
    puts("  -r, -R, --recursive     remove directories and their contents");
    puts("  -f, --force             ignore nonexistent files, never prompt");
    puts("  -d, --dir               remove empty directories without -r");
    puts("  -v, --verbose           explain what is being done");
    puts("      --preserve-root     refuse to operate recursively on '/'");
    puts("      --no-preserve-root  allow recursive removal of '/'");
    puts("      --                  end of options");
    puts("      --help              display this help and exit");
}

int main(int argc, char **argv)
{
    int recursive = 0;
    int force = 0;
    int dir_flag = 0;
    int verbose = 0;
    int preserve_root = 1;
    int help = 0;
    int end_opts = 0;

    const char *operands[256];
    int nops = 0;

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
            if (strcmp(opt, "recursive") == 0)
                recursive = 1;
            else if (strcmp(opt, "force") == 0)
                force = 1;
            else if (strcmp(opt, "dir") == 0)
                dir_flag = 1;
            else if (strcmp(opt, "verbose") == 0)
                verbose = 1;
            else if (strcmp(opt, "preserve-root") == 0)
                preserve_root = 1;
            else if (strcmp(opt, "no-preserve-root") == 0)
                preserve_root = 0;
            else if (strcmp(opt, "help") == 0)
                help = 1;
            else
            {
                printf("rm: unrecognized option '%s'\n", a);
                puts("Try 'rm --help' for more information.");
                sys_exit(1);
                return 1;
            }
            continue;
        }
        /* Single "-" is a file operand (POSIX convention), not a flag. */
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
            {
                if (*p != 'r' && *p != 'R' && *p != 'f' && *p != 'd' &&
                    *p != 'v')
                {
                    bad = *p;
                    break;
                }
            }
            if (!bad)
            {
                for (const char *p = a + 1; *p; p++)
                {
                    if (*p == 'r' || *p == 'R')
                        recursive = 1;
                    else if (*p == 'f')
                        force = 1;
                    else if (*p == 'd')
                        dir_flag = 1;
                    else if (*p == 'v')
                        verbose = 1;
                }
                continue;
            }
            printf("rm: invalid option -- '%c'\n", bad);
            puts("Try 'rm --help' for more information.");
            sys_exit(1);
            return 1;
        }
        if (nops < (int)(sizeof(operands) / sizeof(operands[0])))
            operands[nops++] = a;
        else
        {
            printf("rm: too many operands\n");
            sys_exit(1);
            return 1;
        }
    }

    if (help)
    {
        print_usage();
        sys_exit(0);
        return 0;
    }

    if (nops == 0)
    {
        printf("rm: missing operand\n");
        puts("Try 'rm --help' for more information.");
        sys_exit(1);
        return 1;
    }

    g_force = force;
    g_verbose = verbose;

    int st = 0;
    for (int i = 0; i < nops; i++)
    {
        const char *path = operands[i];

        if (recursive && preserve_root && is_root_path(path))
        {
            printf("rm: it is dangerous to operate recursively on '%s'\n",
                   path);
            puts("use --no-preserve-root to override this failsafe");
            st = 1;
            continue;
        }

        struct stat sb;
        if (stat(path, &sb) < 0)
        {
            if (force && errno == ENOENT)
                continue;
            printf("rm: cannot remove '%s': %s\n", path,
                   rm_strerror(errno));
            st = 1;
            continue;
        }

        if (sb.st_mode & S_IFDIR)
        {
            if (recursive)
            {
                if (rm_recursive(path) < 0)
                    st = 1;
            }
            else if (dir_flag)
            {
                if (rmdir(path) < 0)
                {
                    if (force && errno == ENOENT)
                        continue;
                    printf("rm: cannot remove '%s': %s\n", path,
                           rm_strerror(errno));
                    st = 1;
                }
                else if (verbose)
                    printf("removed directory '%s'\n", path);
            }
            else
            {
                printf("rm: cannot remove '%s': Is a directory\n", path);
                st = 1;
            }
        }
        else
        {
            if (unlink(path) < 0)
            {
                if (force && errno == ENOENT)
                    continue;
                printf("rm: cannot remove '%s': %s\n", path,
                       rm_strerror(errno));
                st = 1;
            }
            else if (verbose)
                printf("removed '%s'\n", path);
        }
    }
    sys_exit(st);
    return st;
}
