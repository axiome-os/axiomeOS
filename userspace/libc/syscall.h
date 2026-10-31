#ifndef AXIOME_LIBC_SYSCALL_H
#define AXIOME_LIBC_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

long syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

/*
 * Syscall numbers come from the single source of truth shared with the kernel
 * (issue #33).  Never redefine them here.
 */
#include "syscall_numbers.h"

/* Open flags (subset of POSIX, must match kernel/vfs.h). */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0100
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

#define DT_FILE   0
#define DT_DIR    1

#define S_IFREG 0x8000
#define S_IFDIR 0x4000

#define FS_RAMFS 0
#define FS_FAT32 1
#define FS_AXIOMEFS 2

struct vfs_dirent {
    unsigned char type;
    char name[256];
};

struct stat {
    unsigned long long st_size;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_ino;
};

int open(const char *path, int flags);
int close(int fd);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);
int mkdir(const char *path);
int unlink(const char *path);
int rmdir(const char *path);
int readdir(const char *path, struct vfs_dirent *ents, int max);
int chdir(const char *path);
int getcwd(char *buf, size_t size);
int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
int mount(int fstype, const char *mountpoint, int dev);
int umount(const char *mountpoint);
int mkfifo(const char *path);
long driver_rescan(void);

long sys_getpid(void);
long sys_fork(void);
long sys_spawn_cmd(const char *cmdline, size_t len);
int execve(const char *path, char *const argv[], char *const envp[]);
long sys_waitpid(int pid, int *status);
long sys_waitpid_nb(int pid, int *status);
int sys_ps(void *buf, int max);
void sys_exit(int code);
long sys_yield(void);

/* ---- system power control (reboot / power-off) ---- */
void sys_reboot(void);
void sys_poweroff(void);

int ipc_create(void);
long ipc_send(int chan, const void *buf, size_t len);
long ipc_recv(int chan, void *buf, size_t max);

long shm_create(size_t bytes);
void *shm_attach(long id);

#define AF_INET  2
#define SOCK_DGRAM  2
#define SOCK_STREAM 1

struct sockaddr {
    unsigned short sa_family;
    char           sa_data[14];
};

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int   sin_addr;
    char           sin_zero[8];
};

int sock_create(int domain, int type, int proto);
int sock_bind(int fd, const struct sockaddr *addr, int addrlen);
int sock_connect(int fd, const struct sockaddr *addr, int addrlen);
int sock_listen(int fd);
int sock_accept(int fd);
long sock_send(int fd, const void *buf, size_t len);
long sock_recv(int fd, void *buf, size_t max);
int sock_close(int fd);

/* ---- DNS / ICMP (Phase 13) ---- */
int dns_resolve(const char *name, uint32_t *ip_out);
int ping(uint32_t dst_be32, uint32_t timeout_ms, uint32_t *rtt_us);

/* Must match kernel/sched.h struct proc_info exactly */
struct proc_info {
    int pid;
    int parent_pid;
    int state;
    int uid;
    char name[16];
};

#define PROC_READY    0
#define PROC_RUNNING  1
#define PROC_BLOCKED  2
#define PROC_ZOMBIE   3

typedef unsigned int uid_t;
typedef unsigned int gid_t;

enum user_role {
    ROLE_GUEST  = 0,
    ROLE_USER   = 1,
    ROLE_ADMIN  = 2,
    ROLE_SYSTEM = 3
};

#define S_ISUID  04000
#define S_ISGID  02000
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getrole(void);
unsigned long long getcap(void);

int setuid(uid_t uid);
int setgid(gid_t gid);
int sys_getpwnam(const char *name, uid_t *uid, gid_t *gid);
int sys_reload_users(void);
int chmod(const char *path, unsigned int mode);
int chown(const char *path, uid_t uid, gid_t gid);

/* read /etc/passwd, verify credentials, return uid or -1 */
int sys_authenticate(const char *user, const char *pass);

/* ---- framebuffer mmap ---- */
long fb_mmap(int fd, uint64_t off, void *virt, size_t len);

/* ---- loadable kernel modules (.kxt) ---- */
long kxtload(const char *path);
long kxtunload(const char *name);

/* ---- system info ---- */
#define UTSNAME_LEN 65
struct utsname {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
    char domainname[UTSNAME_LEN];
};
int uname(struct utsname *buf);

/* ---- file positioning / positional I/O (issue #28) ---- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

long lseek(int fd, long offset, int whence);
long pread(int fd, void *buf, size_t len, long off);
long pwrite(int fd, const void *buf, size_t len, long off);

#endif
