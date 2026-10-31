#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "syscall.h"

static void dump_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("PROC FAIL: open %s -> %d\n", path, fd);
        return;
    }
    char buf[256];
    long n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    printf("%s:\n%s", path, buf);
    if (n > 0 && buf[n - 1] != '\n') printf("\n");
    close(fd);
}

int main(void)
{
    printf("proc_test: starting\n");

    /* ---- 11.6 Named pipe (FIFO) over the VFS namespace ---- */
    if (mkfifo("/mypipe") != 0)
    {
        printf("FIFO FAIL: mkfifo\n");
    }
    else
    {
        long pid = sys_fork();
        if (pid == 0)
        {
            int wfd = open("/mypipe", O_WRONLY);
            if (wfd < 0)
            {
                printf("FIFO child: open write failed\n");
                sys_exit(1);
            }
            const char *msg = "fifo-ok";
            long w = write(wfd, msg, (size_t)strlen(msg));
            close(wfd);
            sys_exit((int)w);
        }
        else
        {
            int rfd = open("/mypipe", O_RDONLY);
            sys_yield();   /* let the child open the write end and write */
            char buf[64];
            long n = read(rfd, buf, sizeof(buf) - 1);
            if (n < 0) n = 0;
            buf[n] = 0;
            if (strcmp(buf, "fifo-ok") == 0)
                printf("FIFO OK: read '%s'\n", buf);
            else
                printf("FIFO FAIL: got '%s'\n", buf);
            close(rfd);
            sys_waitpid((int)pid, 0);
        }
        unlink("/mypipe");
    }

    /* ---- 11.7 /proc virtual filesystem ---- */
    long self = sys_getpid();
    char path[64];
    int k = 0;
    const char *pfx = "/proc/";
    while (pfx[k]) { path[k] = pfx[k]; k++; }
    if (self == 0) path[k++] = '0';
    long tmp = self;
    char dig[16]; int d = 0;
    while (tmp > 0) { dig[d++] = '0' + (tmp % 10); tmp /= 10; }
    while (d > 0) path[k++] = dig[--d];
    path[k++] = '/'; path[k++] = 's'; path[k++] = 't'; path[k++] = 'a';
    path[k++] = 't'; path[k++] = 'u'; path[k++] = 's'; path[k] = 0;
    dump_file(path);
    dump_file("/proc/self/status");
    dump_file("/proc/meminfo");

    printf("PROC /proc listing:\n");
    struct vfs_dirent ents[64];
    int n = readdir("/proc", ents, 64);
    for (int i = 0; i < n; i++)
        printf("  %s%s\n", ents[i].name, ents[i].type == DT_DIR ? "/" : "");

    printf("proc_test: done\n");
    sys_exit(0);
    return 0;
}
