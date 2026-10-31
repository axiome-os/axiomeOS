#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "util.h"

static int opt_recursive = 0;   /* -r / -R */
static int opt_verbose = 0;     /* -v */
static int opt_force = 0;       /* -f */
static int opt_noclobber = 0;   /* -n */

static void usage(void)
{
    puts("Usage: cp [OPTION]... SOURCE... DEST");
    puts("Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.");
    puts("");
    puts("  -f, --force          if an existing destination file cannot be");
    puts("                       opened, remove it and try again");
    puts("  -i, --interactive    prompt before overwrite (not implemented)");
    puts("  -n, --no-clobber     do not overwrite an existing file");
    puts("  -r, -R, --recursive  copy directories recursively");
    puts("  -v, --verbose        explain what is being done");
    puts("      --help           display this help and exit");
}

static int copy_file(const char *src, const char *dst)
{
    if (strcmp(src, dst) == 0)
    {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, dst);
        return -1;
    }
    if (opt_noclobber)
    {
        struct stat s;
        if (stat(dst, &s) == 0)
        {
            if (opt_verbose)
                printf("'%s' -> '%s'\n", src, dst);
            return 0;
        }
    }
    int in = open(src, O_RDONLY);
    if (in < 0)
    {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, ax_strerror(errno));
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
        fprintf(stderr, "cp: cannot create '%s': %s\n",
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
                fprintf(stderr, "cp: write error: %s\n", ax_strerror(errno));
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
        fprintf(stderr, "cp: read error: %s\n", ax_strerror(errno));
        failed = 1;
    }
    close(in);
    close(out);
    if (failed)
    {
        unlink(dst);
        return -1;
    }
    if (opt_verbose)
        printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

/* Copy the contents of directory `src` (already openable) into directory
   `dst` (already created). Returns 0 on success, -1 if anything failed. */
static int copy_contents(const char *src, const char *dst)
{
    int st = 0;
    for (;;)
    {
        struct vfs_dirent ents[64];
        int n = readdir(src, ents, 64);
        if (n < 0)
        {
            fprintf(stderr, "cp: cannot read directory '%s': %s\n",
                    src, ax_strerror(errno));
            return -1;
        }
        int real = 0;
        int changed = 0;
        for (int i = 0; i < n; i++)
        {
            const char *nm = ents[i].name;
            if (nm[0] == '.' &&
                (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            real++;
            char cs[512], cd[512];
            ax_join_path(src, nm, cs, sizeof(cs));
            ax_join_path(dst, nm, cd, sizeof(cd));
            struct stat sb;
            if (stat(cs, &sb) < 0)
            {
                fprintf(stderr, "cp: cannot stat '%s': %s\n",
                        cs, ax_strerror(errno));
                st = -1;
                continue;
            }
            if (sb.st_mode & S_IFDIR)
            {
                if (mkdir(cd) < 0 && errno != EEXIST)
                {
                    fprintf(stderr, "cp: cannot create directory '%s': %s\n",
                            cd, ax_strerror(errno));
                    st = -1;
                    continue;
                }
                if (copy_contents(cs, cd) < 0)
                    st = -1;
                else if (opt_verbose)
                    printf("'%s' -> '%s'\n", cs, cd);
            }
            else
            {
                if (copy_file(cs, cd) < 0)
                    st = -1;
                else
                    changed = 1;
            }
        }
        if (real == 0 || n < 64)
            break;
        if (!changed)
            break;
    }
    return st;
}

/* Copy a directory `src` to `dst`. If dst is an existing directory, the
   copy lands inside it (dst/<basename>); otherwise dst is created as the
   new copy. */
static int copy_dir(const char *src, const char *dst)
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
            fprintf(stderr, "cp: cannot overwrite non-directory '%s' "
                    "with directory '%s'\n", dst, src);
            return -1;
        }
        target = dst;
    }
    if (mkdir(target) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n",
                target, ax_strerror(errno));
        return -1;
    }
    if (copy_contents(src, target) < 0)
        return -1;
    if (opt_verbose)
        printf("'%s' -> '%s'\n", src, target);
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
            if (strcmp(opt, "recursive") == 0) { opt_recursive = 1; continue; }
            if (strcmp(opt, "verbose") == 0) { opt_verbose = 1; continue; }
            if (strcmp(opt, "force") == 0) { opt_force = 1; continue; }
            if (strcmp(opt, "no-clobber") == 0) { opt_noclobber = 1; continue; }
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            fprintf(stderr, "cp: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'r' && *p != 'R' && *p != 'v' && *p != 'f' &&
                    *p != 'n')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "cp: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'cp --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'r' || *p == 'R') opt_recursive = 1;
                else if (*p == 'v') opt_verbose = 1;
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
        fprintf(stderr, "cp: missing destination file operand after '%s'\n",
                nops >= 1 ? ops[nops - 1] : "");
        fprintf(stderr, "Try 'cp --help' for more information.\n");
        sys_exit(1);
    }

    const char *dest = ops[nops - 1];
    struct stat ds;
    int dst_is_dir = (stat(dest, &ds) == 0 && (ds.st_mode & S_IFDIR));

    if (nops > 2 && !dst_is_dir)
    {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dest);
        sys_exit(1);
    }

    int st = 0;
    for (int i = 0; i < nops - 1; i++)
    {
        const char *src = ops[i];
        struct stat sb;
        if (stat(src, &sb) < 0)
        {
            fprintf(stderr, "cp: cannot stat '%s': %s\n",
                    src, ax_strerror(errno));
            st = 1;
            continue;
        }
        if (sb.st_mode & S_IFDIR)
        {
            if (!opt_recursive)
            {
                fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n",
                        src);
                st = 1;
                continue;
            }
            if (copy_dir(src, dest) < 0)
                st = 1;
        }
        else
        {
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
            if (copy_file(src, tdst) < 0)
                st = 1;
        }
    }
    sys_exit(st);
    return st;
}