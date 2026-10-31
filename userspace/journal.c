#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"

#define JOURNAL_PATH "/etc/journal.log"

/* Read the entire open file into a freshly malloc'd buffer. Returns 0 on
   success (sets *out_buf / *out_len; *out_buf may be NULL if empty). */
static int read_whole(int fd, char **out_buf, size_t *out_len)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        *out_buf = 0;
        *out_len = 0;
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    if (sz == 0)
    {
        *out_buf = 0;
        *out_len = 0;
        return 0;
    }
    char *buf = (char *)malloc(sz);
    if (!buf)
    {
        *out_buf = 0;
        *out_len = 0;
        return -1;
    }
    size_t got = 0;
    while (got < sz)
    {
        long r = read(fd, buf + got, sz - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    *out_buf = buf;
    *out_len = got;
    return 0;
}

/* Print the whole file to stdout. */
static void dump_all(int fd)
{
    char buf[512];
    long r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        for (long i = 0; i < r; i++)
            putchar((unsigned char)buf[i]);
}

/* Print only the last `n` lines of the file. */
static void dump_tail(int fd, int n)
{
    char *buf;
    size_t len;
    if (read_whole(fd, &buf, &len) != 0 || len == 0)
        return;

    long start = 0;
    if (n > 0)
    {
        long lines = 0;
        for (long i = (long)len - 1; i >= 0; i--)
        {
            if (buf[i] == '\n')
            {
                lines++;
                if (lines == n)
                {
                    start = i + 1;
                    break;
                }
            }
        }
    }
    for (size_t i = (size_t)start; i < len; i++)
        putchar((unsigned char)buf[i]);
    free(buf);
}

int main(int argc, char **argv)
{
    int n = -1;        /* -1 => all lines */
    int do_clear = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "clear") == 0)
            do_clear = 1;
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            n = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] == 'n' && argv[i][2])
            n = atoi(argv[i] + 2);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("usage: journal [clear] [-n N]\n");
            sys_exit(0);
        }
        else
        {
            printf("journal: unknown argument '%s'\n", argv[i]);
            sys_exit(1);
        }
    }

    if (do_clear)
    {
        if (journal_clear() != 0)
        {
            printf("journal: clear failed\n");
            sys_exit(1);
        }
        sys_exit(0);
    }

    int fd = open(JOURNAL_PATH, O_RDONLY);
    if (fd < 0)
    {
        printf("journal: cannot open %s\n", JOURNAL_PATH);
        sys_exit(1);
    }

    if (n < 0)
        dump_all(fd);
    else
        dump_tail(fd, n);

    close(fd);
    sys_exit(0);
    return 0;
}
