#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"
#include "util.h"

#ifndef S_IRWXU
#define S_IRWXU 00700
#define S_IRWXG 00070
#define S_IRWXO 00007
#endif

static int opt_recursive = 0;   /* -R */
static int opt_verbose = 0;     /* -v */

static void usage(void)
{
    puts("Usage: chmod [OPTION]... MODE[,MODE]... FILE...");
    puts("Change the mode of each FILE to MODE.");
    puts("");
    puts("  -R, --recursive     change files and directories recursively");
    puts("  -v, --verbose       explain what is being done");
    puts("      --help          display this help and exit");
    puts("");
    puts("MODE may be octal (e.g. 755) or symbolic (e.g. u+x, a-w).");
}

/* Per-group permission masks (u=0, g=1, o=2). */
static unsigned int base_mask(int g)
{
    return (g == 0) ? (S_IRWXU | S_ISUID) :
           (g == 1) ? (S_IRWXG | S_ISGID) :
                      S_IRWXO;
}

static unsigned int rwx_bit(int g, char c)
{
    switch (c)
    {
    case 'r': return (g == 0) ? S_IRUSR : (g == 1) ? S_IRGRP : S_IROTH;
    case 'w': return (g == 0) ? S_IWUSR : (g == 1) ? S_IWGRP : S_IWOTH;
    case 'x': return (g == 0) ? S_IXUSR : (g == 1) ? S_IXGRP : S_IXOTH;
    case 's': return (g == 0) ? S_ISUID : (g == 1) ? S_ISGID : 0;
    default: return 0;
    }
}

/* Apply one "u+x" style clause to *m. Returns 0 on success, -1 on a syntax
   error. */
static int apply_clause(unsigned int *m, const char *s)
{
    unsigned int who = 0;
    while (*s == 'u' || *s == 'g' || *s == 'o' || *s == 'a')
    {
        char c = *s++;
        if (c == 'u') who |= 1;
        else if (c == 'g') who |= 2;
        else if (c == 'o') who |= 4;
        else who |= 7;
    }
    if (!who)
        who = 7; /* no who: same as 'a' */
    char op = *s++;
    if (op != '+' && op != '-' && op != '=')
        return -1;

    unsigned int bits[3] = {0, 0, 0};
    while (*s)
    {
        char c = *s++;
        if (c != 'r' && c != 'w' && c != 'x' && c != 's' && c != 'X')
            return -1;
        for (int g = 0; g < 3; g++)
        {
            if (!(who & (1 << g)))
                continue;
            if (c == 'X')
            {
                /* execute only if the file is already executable (or a dir;
                   dirs are handled by the caller passing S_IFDIR). */
                if ((*m & rwx_bit(g, 'x')) || (*m & S_IFDIR))
                    bits[g] |= rwx_bit(g, 'x');
            }
            else
            {
                bits[g] |= rwx_bit(g, c);
            }
        }
    }
    for (int g = 0; g < 3; g++)
    {
        if (!(who & (1 << g)))
            continue;
        if (op == '=')
            *m &= ~base_mask(g);
        else if (op == '-')
            *m &= ~bits[g];
        if (op != '-')
            *m |= bits[g];
    }
    return 0;
}

/* Parse a mode string (octal or comma-separated symbolic clauses). Returns 0
   and stores the new mode in *m on success; -1 on an invalid mode. */
static int parse_mode(const char *orig, unsigned int *m)
{
    int octal = 1;
    for (const char *p = orig; *p; p++)
        if (*p < '0' || *p > '7')
        {
            octal = 0;
            break;
        }
    if (octal)
    {
        unsigned int v = 0;
        for (const char *p = orig; *p; p++)
            v = v * 8 + (unsigned int)(*p - '0');
        *m = (unsigned int)((*m & ~07777u) | (v & 07777u));
        return 0;
    }

    char buf[64];
    size_t n = strlen(orig);
    if (n >= sizeof(buf))
        return -1;
    memcpy(buf, orig, n + 1);
    char *seg = buf;
    while (*seg)
    {
        char *comma = strchr(seg, ',');
        if (comma)
            *comma = 0;
        if (apply_clause(m, seg) < 0)
            return -1;
        if (!comma)
            break;
        seg = comma + 1;
    }
    return 0;
}

static void oct4(unsigned int v)
{
    char b[5];
    b[4] = 0;
    b[3] = (char)('0' + (v & 7)); v >>= 3;
    b[2] = (char)('0' + (v & 7)); v >>= 3;
    b[1] = (char)('0' + (v & 7)); v >>= 3;
    b[0] = (char)('0' + (v & 7));
    printf("%s", b);
}

static void report(const char *path, unsigned int oldmode, unsigned int newmode)
{
    char o[11], n[11];
    ax_mode_str(oldmode | S_IFREG, o);
    ax_mode_str(newmode | S_IFREG, n);
    printf("mode of '%s' changed from ", path);
    oct4(oldmode & 07777);
    printf(" (%s) to ", o + 1);
    oct4(newmode & 07777);
    printf(" (%s)\n", n + 1);
}

static int chmod_one(const char *path, const char *mode_str)
{
    struct stat sb;
    if (stat(path, &sb) < 0)
    {
        fprintf(stderr, "chmod: cannot access '%s': %s\n",
                path, ax_strerror(errno));
        return -1;
    }
    unsigned int m = sb.st_mode;
    if (parse_mode(mode_str, &m) < 0)
        return -1;
    unsigned int old = sb.st_mode & 07777;
    if (chmod(path, m) < 0)
    {
        fprintf(stderr, "chmod: cannot change mode of '%s': %s\n",
                path, ax_strerror(errno));
        return -1;
    }
    if (opt_verbose)
        report(path, old, m & 07777);
    return 0;
}

static int chmod_rec(const char *path, const char *mode_str)
{
    int st = chmod_one(path, mode_str);
    struct stat sb;
    if (st < 0 || stat(path, &sb) < 0 || !(sb.st_mode & S_IFDIR))
        return st;
    for (;;)
    {
        struct vfs_dirent ents[64];
        int n = readdir(path, ents, 64);
        if (n < 0)
            break;
        int real = 0, changed = 0;
        for (int i = 0; i < n; i++)
        {
            const char *nm = ents[i].name;
            if (nm[0] == '.' &&
                (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            real++;
            char child[512];
            ax_join_path(path, nm, child, sizeof(child));
            if (chmod_rec(child, mode_str) < 0)
                st = -1;
            else
                changed = 1;
        }
        if (real == 0 || n < 64)
            break;
        if (!changed)
            break;
    }
    return st;
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
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            fprintf(stderr, "chmod: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (!end_opts && a[0] == '-' && a[1] != 0)
        {
            char bad = 0;
            for (const char *p = a + 1; *p; p++)
                if (*p != 'R' && *p != 'v')
                {
                    bad = *p;
                    break;
                }
            if (bad)
            {
                fprintf(stderr, "chmod: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'chmod --help' for more information.\n");
                sys_exit(1);
            }
            for (const char *p = a + 1; *p; p++)
            {
                if (*p == 'R') opt_recursive = 1;
                else if (*p == 'v') opt_verbose = 1;
            }
            continue;
        }
        if (nops < (int)(sizeof(ops) / sizeof(ops[0])))
            ops[nops++] = a;
    }

    if (nops < 2)
    {
        fprintf(stderr, "chmod: missing operand\n");
        fprintf(stderr, "Try 'chmod --help' for more information.\n");
        sys_exit(1);
    }

    const char *mode_str = ops[0];
    unsigned int dm = 0644;
    if (parse_mode(mode_str, &dm) < 0)
    {
        fprintf(stderr, "chmod: invalid mode: '%s'\n", mode_str);
        sys_exit(1);
    }

    int st = 0;
    for (int i = 1; i < nops; i++)
    {
        if (opt_recursive)
        {
            if (chmod_rec(ops[i], mode_str) < 0)
                st = 1;
        }
        else
        {
            if (chmod_one(ops[i], mode_str) < 0)
                st = 1;
        }
    }
    sys_exit(st);
    return st;
}