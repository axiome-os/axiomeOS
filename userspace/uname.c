#include "syscall.h"
#include "stdio.h"
#include "string.h"

static const char *fld(char s[UTSNAME_LEN])
{
    return s[0] ? s : (const char *)"unknown";
}

static void usage(void)
{
    puts("Usage: uname [OPTION]...");
    puts("Print certain system information.  With no OPTION, same as -s.");
    puts("");
    puts("  -a, --all                print all information");
    puts("  -s, --kernel-name        print the kernel name");
    puts("  -n, --nodename           print the network node hostname");
    puts("  -r, --kernel-release     print the kernel release");
    puts("  -v, --kernel-version     print the kernel version");
    puts("  -m, --machine            print the machine hardware name");
    puts("  -o, --operating-system   print the operating system");
    puts("      --help               display this help and exit");
}

int main(int argc, char **argv)
{
    struct utsname u;
    if (uname(&u) < 0)
    {
        fprintf(stderr, "uname: uname(2) failed\n");
        sys_exit(1);
    }

    int all = 0, sname = 0, node = 0, rel = 0, ver = 0, mach = 0, os = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) { usage(); sys_exit(0); }
        if (strcmp(a, "--version") == 0)
        {
            puts("uname (axiomeOS) 1.0");
            sys_exit(0);
        }
        if (a[0] == '-' && a[1] == '-' && a[2] != 0)
        {
            const char *opt = a + 2;
            if (strcmp(opt, "all") == 0) { all = 1; continue; }
            if (strcmp(opt, "kernel-name") == 0) { sname = 1; continue; }
            if (strcmp(opt, "nodename") == 0) { node = 1; continue; }
            if (strcmp(opt, "kernel-release") == 0) { rel = 1; continue; }
            if (strcmp(opt, "kernel-version") == 0) { ver = 1; continue; }
            if (strcmp(opt, "machine") == 0) { mach = 1; continue; }
            if (strcmp(opt, "operating-system") == 0) { os = 1; continue; }
            fprintf(stderr, "uname: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        if (a[0] == '-' && a[1] != 0 && a[1] != '-')
        {
            for (const char *c = a + 1; *c; c++)
            {
                switch (*c)
                {
                case 'a': all = 1; break;
                case 's': sname = 1; break;
                case 'n': node = 1; break;
                case 'r': rel = 1; break;
                case 'v': ver = 1; break;
                case 'm': mach = 1; break;
                case 'o': os = 1; break;
                default:
                    fprintf(stderr, "uname: invalid option -- '%c'\n", *c);
                    fprintf(stderr,
                            "Try 'uname --help' for more information.\n");
                    sys_exit(1);
                }
            }
            continue;
        }
        if (a[0] == '-' && a[1] == 0)
        {
            /* lone "-" is accepted and ignored like GNU uname */
            continue;
        }
        fprintf(stderr, "uname: extra operand '%s'\n", a);
        sys_exit(1);
    }

    if (all)
        sname = node = rel = ver = mach = os = 1;
    if (!sname && !node && !rel && !ver && !mach && !os)
        sname = 1;

    if (sname) printf("%s\n", fld(u.sysname));
    if (node)  printf("%s\n", fld(u.nodename));
    if (rel)   printf("%s\n", fld(u.release));
    if (ver)   printf("%s\n", fld(u.version));
    if (mach)  printf("%s\n", fld(u.machine));
    if (os)    printf("%s\n", fld(u.sysname));
    sys_exit(0);
    return 0;
}