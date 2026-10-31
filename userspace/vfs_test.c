#include "syscall.h"
#include "string.h"

/* All diagnostics go to fd 2 (console) so the dup2 test (which redirects
   fd 1 into a pipe) never hides our output. */
static void log(const char *s) { write(2, s, strlen(s)); }
static void logd(long v)
{
    char buf[24]; int i = 0;
    if (v < 0) { log("-"); v = -v; }
    if (v == 0) buf[i++] = '0';
    else { char t[24]; int j = 0; while (v) { t[j++] = '0' + (v % 10); v /= 10; }
           while (j) buf[i++] = t[--j]; }
    buf[i] = 0; log(buf);
}
static void logline(const char *s) { log(s); log("\n"); }

static void list_dir(const char *path)
{
    struct vfs_dirent ents[64];
    int n = readdir(path, ents, 64);
    log("ls "); log(path); log(" ("); logd(n); logline("):");
    for (int i = 0; i < n; i++)
    {
        log("  "); log(ents[i].type == DT_DIR ? "d " : "f ");
        log(ents[i].name); logline("");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* ---- pipe ---- */
    int fds[2];
    if (pipe(fds) != 0) { logline("FAIL pipe"); sys_exit(1); }
    const char *msg = "hello-pipe";
    int w = write(fds[1], msg, strlen(msg));
    char buf[64];
    int n = read(fds[0], buf, sizeof(buf) - 1);
    buf[n] = 0;
    log("pipe: wrote="); logd(w); log(" read="); logd(n);
    log(" '"); log(buf); logline("'");
    close(fds[1]); close(fds[0]);

    /* ---- dup2 redirecting stdout into a pipe ---- */
    int pf[2];
    pipe(pf);
    int d = dup2(pf[1], 1);
    log("dup2 ret="); logd(d); logline("");
    write(1, "via-dup2", 8);
    close(pf[1]);
    char b2[32];
    int m = read(pf[0], b2, sizeof(b2) - 1);
    b2[m] = 0;
    log("dup2: read='"); log(b2); logline("'");
    close(pf[0]);

    /* ---- mount a ramfs, write/read a file, unmount ---- */
    if (mount(FS_RAMFS, "/mnt", 0) != 0) { logline("FAIL mount /mnt"); }
    else
    {
        int fd = open("/mnt/f.txt", O_CREAT | O_WRONLY);
        write(fd, "ramfs-ok", 8);
        close(fd);
        fd = open("/mnt/f.txt", O_RDONLY);
        char rb[32]; int rn = read(fd, rb, sizeof(rb) - 1); rb[rn] = 0; close(fd);
        log("mount: /mnt/f.txt='"); log(rb); log("' ("); logd(rn); logline(" bytes)");
        int u = umount("/mnt");
        log("umount /mnt ret="); logd(u); logline("");
        int after = open("/mnt/f.txt", O_RDONLY);
        log("after-umount open="); logd(after); logline(" (expect -1)");
    }

    /* ---- FAT32: unmount /boot, remount the partition at /boot2 ---- */
    if (umount("/boot") == 0)
    {
        logline("unmounted /boot");
        if (mount(FS_FAT32, "/boot2", 0) == 0)
        {
            int fd = open("/boot2/HELLO.TXT", O_RDONLY);
            char hb[64]; int hn = read(fd, hb, sizeof(hb) - 1); hb[hn] = 0; close(fd);
            log("fat32 remount: /boot2/HELLO.TXT='"); log(hb); logline("'");
            log("umount /boot2 ret="); logd(umount("/boot2")); logline("");
        }
        else
            logline("FAIL mount /boot2");
    }
    else
        logline("SKIP fat32 remount (busy /boot)");

    /* ---- axiomefs: mount the CoW block device at /axfs, test ops ----
       Disk is the primary-slave IDE drive => dev = (bus<<16)|(drive<<8)|part
       = (0<<16)|(1<<8)|0 = 256. */
    {
        int dev = (0 << 16) | (1 << 8) | 0;
        if (mount(FS_AXIOMEFS, "/axfs", dev) != 0)
            logline("FAIL mount /axfs");
        else
        {
            list_dir("/axfs");

            int fd = open("/axfs/README.TXT", O_RDONLY);
            char rb[64]; int rn = read(fd, rb, sizeof(rb) - 1); rb[rn] = 0; close(fd);
            log("axfs: README.TXT='"); log(rb); log("' ("); logd(rn); logline(" bytes)");

            if (mkdir("/axfs/sub") != 0) logline("FAIL axfs mkdir /axfs/sub");

#if 1
            fd = open("/axfs/new.txt", O_CREAT | O_WRONLY);
            int w = write(fd, "axiomefs-write", 14); close(fd);
            log("axfs: wrote new.txt="); logd(w); logline(" bytes");

            fd = open("/axfs/new.txt", O_RDONLY);
            char rb2[64]; int rn2 = read(fd, rb2, sizeof(rb2) - 1); rb2[rn2] = 0;
            close(fd);
            log("axfs: new.txt='"); log(rb2); log("' ("); logd(rn2); logline(" bytes)");

            list_dir("/axfs");

            if (unlink("/axfs/new.txt") != 0) logline("FAIL axfs unlink new.txt");
            list_dir("/axfs");
#endif

            log("umount /axfs ret="); logd(umount("/axfs")); logline("");
            int after = open("/axfs/README.TXT", O_RDONLY);
            log("after-umount axfs open="); logd(after); logline(" (expect -1)");
        }
    }

    sys_exit(0);
    return 0;
}
