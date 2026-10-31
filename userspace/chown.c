#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"
#include "util.h"

static int opt_recursive = 0;   /* -R */
static int opt_verbose = 0;     /* -v */

static void usage(void)
{
    puts("Usage: chown [OPTION]... [OWNER][:[GROUP]] FILE...");
    puts("Change the owner and/or group of each FILE to OWNER and/or GROUP.");
    puts("");
    puts("  -R, --recursive     operate on files and directories recursively");
    puts("  -v, --verbose       explain what is being done");
    puts("      --help          display this help and exit");
    puts("");
    puts("OWNER and GROUP may be a user/group name or a numeric ID.");
}

/* Resolve a user name through the kernel user database, falling back to a
   numeric uid. Returns 0 on success. */
static int resolve_user(const char *name, uid_t *uid, gid_t *gid)
{
    if (sys_getpwnam(name, uid, gid) == 0)
        return 0;
    if (name[0] >= '0' && name[0] <= '9')
    {
        *uid = (uid_t)atoi(name);
        *gid = 0;
        return 0;
    }
    return -1;
}

static int resolve_group(const char *name, gid_t *gid)
{
    if (name[0] >= '0' && name[0] <= '9')
    {
        *gid = (gid_t)atoi(name);
        return 0;
    }
    uid_t u;
    gid_t g;
    if (sys_getpwnam(name, &u, &g) == 0)
    {
        *gid = g;
        return 0;
    }
    return -1;
}

static int chown_one(const char *path, uid_t uid, gid_t gid)
{
    struct stat sb;
    if (stat(path, &sb) < 0)
    {
        fprintf(stderr, "chown: cannot access '%s': %s\n",
                path, ax_strerror(errno));
        return -1;
    }
    if (chown(path, uid, gid) < 0)
    {
        fprintf(stderr, "chown: cannot change ownership of '%s': %s\n",
                path, ax_strerror(errno));
        return -1;
    }
    if (opt_verbose)
        printf("changed ownership of '%s' to %u:%u\n",
               path, (unsigned int)uid, (unsigned int)gid);
    return 0;
}

static int chown_rec(const char *path, uid_t uid, gid_t gid)
{
    int st = chown_one(path, uid, gid);
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
            if (chown_rec(child, uid, gid) < 0)
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
            fprintf(stderr, "chown: unrecognized option '%s'\n", a);
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
                fprintf(stderr, "chown: invalid option -- '%c'\n", bad);
                fprintf(stderr, "Try 'chown --help' for more information.\n");
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
        fprintf(stderr, "chown: missing operand\n");
        fprintf(stderr, "Try 'chown --help' for more information.\n");
        sys_exit(1);
    }

    const char *spec = ops[0];
    char own[64], grp[64];
    int has_grp = 0;
    size_t sn = strlen(spec);
    if (sn >= sizeof(own))
    {
        fprintf(stderr, "chown: invalid ownership spec '%s'\n", spec);
        sys_exit(1);
    }
    memcpy(own, spec, sn + 1);
    char *colon = strchr(own, ':');
    if (colon)
    {
        *colon = 0;
        memcpy(grp, colon + 1, strlen(colon + 1) + 1);
        has_grp = 1;
    }

    uid_t uid = 0;
    gid_t gid = 0;
    if (own[0] == 0)
    {
        fprintf(stderr, "chown: no owner specified (group-only chown is not "
                "supported: the VFS stat has no owner field)\n");
        sys_exit(1);
    }
    if (resolve_user(own, &uid, &gid) < 0)
    {
        fprintf(stderr, "chown: invalid user: '%s'\n", own);
        sys_exit(1);
    }
    if (has_grp)
    {
        if (resolve_group(grp, &gid) < 0)
        {
            fprintf(stderr, "chown: invalid group: '%s'\n", grp);
            sys_exit(1);
        }
    }

    int st = 0;
    for (int i = 1; i < nops; i++)
    {
        if (opt_recursive)
        {
            if (chown_rec(ops[i], uid, gid) < 0)
                st = 1;
        }
        else
        {
            if (chown_one(ops[i], uid, gid) < 0)
                st = 1;
        }
    }
    sys_exit(st);
    return st;
}