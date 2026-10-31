#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"

#define PASSWD_PATH "/etc/passwd"
#define PASSWD_MAX  8192

/* whoami: print the username associated with the effective user ID. */

static char *read_whole(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        *len_out = 0;
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0)
    {
        close(fd);
        *len_out = 0;
        return 0;
    }
    size_t sz = (size_t)st.st_size;
    if (sz > PASSWD_MAX) sz = PASSWD_MAX;
    char *buf = (char *)malloc(sz + 1);
    if (!buf)
    {
        close(fd);
        *len_out = 0;
        return 0;
    }
    size_t got = 0;
    while (got < sz)
    {
        long r = read(fd, buf + got, sz - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    buf[got] = 0;
    close(fd);
    *len_out = got;
    return buf;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0)
            continue;
        if (strcmp(a, "--help") == 0)
        {
            puts("Usage: whoami [OPTION]...");
            puts("Print the user name associated with the current effective"
                 " user ID.");
            puts("      --help     display this help and exit");
            sys_exit(0);
        }
        if (strcmp(a, "--version") == 0)
        {
            puts("whoami (axiomeOS) 1.0");
            sys_exit(0);
        }
        if (a[0] == '-')
        {
            fprintf(stderr, "whoami: invalid option -- '%c'\n", a[1]);
            sys_exit(1);
        }
    }

    uid_t euid = geteuid();

    size_t len;
    char *text = read_whole(PASSWD_PATH, &len);
    if (!text)
    {
        fprintf(stderr, "whoami: cannot open %s\n", PASSWD_PATH);
        sys_exit(1);
    }

    char *found = 0;
    char *line = text;
    while (*line)
    {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        char saved = *nl;
        *nl = 0;
        char *f[7];
        int nf = 0;
        char *p = line;
        while (*p && nf < 7)
        {
            f[nf++] = p;
            while (*p && *p != ':') p++;
            if (*p) { *p = 0; p++; }
        }
        if (nf >= 3)
        {
            long u = 0;
            const char *q = f[2];
            while (*q >= '0' && *q <= '9') u = u * 10 + (long)(*q++ - '0');
            if ((uid_t)u == euid)
            {
                found = f[0];
                break;
            }
        }
        *nl = saved;
        if (!*nl)
            break;
        line = nl + 1;
    }

    if (found)
    {
        printf("%s\n", found);
        free(text);
        sys_exit(0);
    }

    fprintf(stderr, "whoami: cannot find name for user ID %u\n",
            (unsigned int)euid);
    free(text);
    sys_exit(1);
    return 1;
}