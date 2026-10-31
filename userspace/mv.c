#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "util.h"

static int opt_verbose = 0;     /* -v */
static int opt_force = 0;       /* -f */
static int opt_noclobber = 0;   /* -n */

static void usage(void)
{
    puts("Usage: mv [OPTION]... SOURCE... DEST");
    puts("Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.");
    puts("");
    puts("  -f, --force         do not prompt before overwriting");
    puts("  -n, --no-clobber    do not overwrite an existing file");
    puts("  -v, --verbose       explain what is being done");
    puts("      --help          display this help and exit");
}

static int copy_file(const char *src, const char *dst)
{
    if (strcmp(src, dst) == 0)
        return 0;
    if (opt_noclobber)
    {
        struct stat s;
        if (stat(dst, &s) == 0)
            return 1; /* leave in place, not an error */
    }
    int in = open(src, O_RDONLY);
    if (in < 0)
    {
        fprintf(stderr, "mv: cannot open '%s': %s\n", src, ax_strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0 && opt_force)
    {
        unlink(dst);
        out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    }
    if (out < 0)
    {
        close(in);
        fprintf(stderr, "mv: cannot create '%s': %s\n",
                dst, ax_strerror(errno));
        return -1;
    }
    char buf[512];
    long r;
    int failed = 0;
    while ((r = read(in, buf, sizeof(buf))) > 0)
    {
        size_t off = 0;
        while ((size_t)off < (size_t)r)
        {
            long w = write(out, buf + off, (size_t)r - off);
            if (w < 0)
            {
                fprintf(stderr, "mv: write error: %s\n", ax_strerror(errno));
                failed = 1;
                break;
            }
            off += (size_t)w;
        }
        if (failed)
            break;
    }
    if (r < 0)
    {
        fprintf(stderr, "mv: read error: %s\n", ax_strerror(errno));
        failed = 1;
    }
    close(in);
    close(out);
    if (failed)
    {
        unlink(dst);
        return -1;
    }
    if (unlink(src) < 0)
    {
        fprintf(stderr, "mv: cannot remove '%s': %s\n", src, ax_strerror(errno));
        unlink(dst);
        return -1;
    }
    if (opt_verbose)
        printf("renamed '%s' -> '%s'\n", src, dst);
    return 0;
}

/* Remove a directory tree (after a move). */
static int remove_tree(const char *path)
{
    struct stat sb;
    if (stat(path, &sb) < 0)
        return -1;
    if (!(sb.st_mode & S_IFDIR))
    {
        return unlink(path) < 0 ? -1 : 0;
    }
    for (;;)
    {
        struct vfs_dirent ents[64];
        int n = readdir(path, ents, 64);
        if (n < 0)
            return -1;
        int real = 0;
        int changed = 0;
        for (int i = 0; i < n; i++)
        {
            const char *nm = ents[i].name;
            if (nm[0] == '.' &&
                (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            real++;
            char child[512];
            ax_join_path(path, nm, child, sizeof(child));
            if (remove_tree(child) == 0)
                changed = 1;
        }
        if (real == 0 || n < 64)
            break;
        if (!changed)
            break;
    }
    if (rmdir(path) < 0)
        return -1;
    return 0;
}

/* Copy a directory `src` to `dst` and remove `src`. */
static int move_dir(const char *src, const char *dst)
{
    char real[512];
    const char *target;
    struct stat ds;
    if (stat(dst, &ds) == 0 && (ds.st_mode & S_IFDIR))
    {
        ax_join_path(dst, ax_basename(src), real, sizeof(real));
        target = real;
    }
    else
    {
        if (stat(dst, &ds) == 0)
        {
            fprintf(stderr, "mv: cannot overwrite non-directory '%s' "
                    "with directory '%s'\n", dst, src);
            return -1;
        }
        if (opt_noclobber)
        {
            struct stat s;
            if (stat(dst, &s) == 0)
                return 1;
        }
        target = dst;
    }
    if (mkdir(target) < 0)
    {
        if (errno != EEXIST)
        {
            fprintf(stderr, "mv: cannot create directory '%s': %s\n",
                    target, ax_strerror(errno));
            return -1;
        }
        struct stat s;
        if (stat(target, &s) == 0 && !(s.st_mode & S_IFDIR))
        {
            fprintf(stderr, "mv: cannot overwrite non-directory '%s' "
                    "with directory '%s'\n", target, src);
            return -1;
        }
    }
    if (strcmp(target, src) == 0)
        return 0;

    /* Recursively copy contents. */
    struct vfs_dirent ents[64];
    int n = readdir(src, ents, 64);
    if (n < 0)
    {
        fprintf(stderr, "mv: cannot read directory '%s': %s\n",
                src, ax_strerror(errno));
        return -1;
    }
    int failed = 0;
    for (int i = 0; i < n && i < 64; i++)
    {
        const char *nm = ents[i].name;
        if (nm[0] == '.' &&
            (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        char cs[512], cd[512];
        ax_join_path(src, nm, cs, sizeof(cs));
        ax_join_path(target, nm, cd, sizeof(cd));
        struct stat sb;
        if (stat(cs, &sb) < 0)
        {
            fprintf(stderr, "mv: cannot stat '%s': %s\n",
                    cs, ax_strerror(errno));
            failed = 1;
            continue;
        }
        if (sb.st_mode & S_IFDIR)
        {
            if (move_dir(cs, cd) < 0)
                failed = 1;
        }
        else
        {
            if (copy_file(cs, cd) < 0)
                failed = 1;
        }
    }
    if (failed)
        return -1;
    if (remove_tree(src) < 0)
    {
        fprintf(stderr, "mv: cannot remove '%s': %s\n", src, ax_strerror(errno));
        return -1;
    }
    if (opt_verbose)
        printf("renamed '%s' -> '%s'\n", src, target);
    return 0;
}

int main(int argc, char **argv)
{
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
            if (strcmp(opt, "verbose") == 0) { opt_verbose = 1; continue; }
            if (strcmp(opt, "force") == 0) { opt_force = 1; continue; }
            if (strcmp(opt, "no-clobber") == 0) { opt_noclobber = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            fprintf(stderr, "mv: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'v' && *p != 'f' && *p != 'n')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "mv: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'mv --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'v') opt_verbose = 1;
                else if (*p == 'f') opt_force = 1;
                else if (*p == 'n') opt_noclobber = 1;
            }
            continue;
        }
        if (nops < (int)(sizeof(ops) / sizeof(ops[0])))
            ops[nops++] = a;
    }

    if (nops < 2)
    {
        fprintf(stderr, "mv: missing destination file operand after '%s'\n",
                nops >= 1 ? ops[nops - 1] : "");
        fprintf(stderr, "Try 'mv --help' for more information.\n");
        sys_exit(1);
    }

    const char *dest = ops[nops - 1];
    struct stat ds;
    int dst_is_dir = (stat(dest, &ds) == 0 && (ds.st_mode & S_IFDIR));

    if (nops > 2 && !dst_is_dir)
    {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dest);
        sys_exit(1);
    }

    int st = 0;
    for (int i = 0; i < nops - 1; i++)
    {
        const char *src = ops[i];
        struct stat sb;
        if (stat(src, &sb) < 0)
        {
            fprintf(stderr, "mv: cannot stat '%s': %s\n",
                    src, ax_strerror(errno));
            st = 1;
            continue;
        }
        char target[512];
        const char *tdst;
        if (dst_is_dir)
        {
            ax_join_path(dest, ax_basename(src), target, sizeof(target));
            tdst = target;
        }
        else
        {
            tdst = dest;
        }
        if (sb.st_mode & S_IFDIR)
        {
            if (move_dir(src, tdst) < 0)
                st = 1;
        }
        else
        {
            if (copy_file(src, tdst) < 0)
                st = 1;
        }
    }
    sys_exit(st);
    return st;
}