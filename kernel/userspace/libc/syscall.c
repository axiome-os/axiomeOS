#include "syscall.h"
#include "errno.h"
#include <stddef.h>

long syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r0 __asm__("rax") = n;
    register long r1 __asm__("rdi") = a1;
    register long r2 __asm__("rsi") = a2;
    register long r3 __asm__("rdx") = a3;
    register long r4 __asm__("r10") = a4;
    register long r5 __asm__("r8")  = a5;
    register long r6 __asm__("r9")  = a6;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "r"(r0), "r"(r1), "r"(r2), "r"(r3),
                        "r"(r4), "r"(r5), "r"(r6)
                      : "%rcx", "%r11", "memory");
    
    /* Handle negative errno values from kernel */
    if (ret < 0 && ret >= -4095)
    {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}

int open(const char *path, int flags)
{
    return (int)syscall(SYS_OPEN, (long)path, (long)flags, 0, 0, 0, 0);
}

int close(int fd)
{
    return (int)syscall(SYS_CLOSE, (long)fd, 0, 0, 0, 0, 0);
}

long write(int fd, const void *buf, size_t len)
{
    return syscall(SYS_WRITE, (long)fd, (long)buf, (long)len, 0, 0, 0);
}

long read(int fd, void *buf, size_t len)
{
    return syscall(SYS_READ, (long)fd, (long)buf, (long)len, 0, 0, 0);
}

int mkdir(const char *path)
{
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0, 0, 0, 0);
}

int unlink(const char *path)
{
    return (int)syscall(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0);
}

int rmdir(const char *path)
{
    return (int)syscall(SYS_RMDIR, (long)path, 0, 0, 0, 0, 0);
}

int readdir(const char *path, struct vfs_dirent *ents, int max)
{
    return (int)syscall(SYS_READDIR, (long)path, (long)ents, (long)max, 0, 0, 0);
}

int chdir(const char *path)
{
    return (int)syscall(SYS_CHDIR, (long)path, 0, 0, 0, 0, 0);
}

int getcwd(char *buf, size_t size)
{
    return (int)syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0, 0);
}

int fstat(int fd, struct stat *st)
{
    return (int)syscall(SYS_FSTAT, (long)fd, (long)st, 0, 0, 0, 0);
}

int stat(const char *path, struct stat *st)
{
    /* Use the path-based stat syscall so this works on directories too
       (open(path, O_RDONLY) would reject a directory with EISDIR). */
    return (int)syscall(SYS_STAT, (long)path, (long)st, 0, 0, 0, 0);
}

int dup2(int oldfd, int newfd)
{
    return (int)syscall(SYS_DUP2, (long)oldfd, (long)newfd, 0, 0, 0, 0);
}

int pipe(int fds[2])
{
    return (int)syscall(SYS_PIPE, (long)fds, 0, 0, 0, 0, 0);
}

int mount(int fstype, const char *mountpoint, int dev)
{
    return (int)syscall(SYS_MOUNT, (long)fstype, (long)mountpoint, (long)dev, 0, 0, 0);
}

int umount(const char *mountpoint)
{
    return (int)syscall(SYS_UMOUNT, (long)mountpoint, 0, 0, 0, 0, 0);
}

int mkfifo(const char *path)
{
    return (int)syscall(SYS_MKFIFO, (long)path, 0, 0, 0, 0, 0);
}

long driver_rescan(void)
{
    return syscall(SYS_DRIVER_RESCAN, 0, 0, 0, 0, 0, 0);
}

long sys_getpid(void) { return syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0); }
long sys_yield(void) { return syscall(SYS_YIELD, 0, 0, 0, 0, 0, 0); }
long sys_fork(void)   { return syscall(SYS_FORK,   0, 0, 0, 0, 0, 0); }
long sys_spawn_cmd(const char *cmdline, size_t len)
{
    return syscall(SYS_SPAWN_CMD, (long)cmdline, (long)len, 0, 0, 0, 0);
}
int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)syscall(SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0);
}
long sys_waitpid(int pid, int *status)
{
    return syscall(SYS_WAITPID, (long)pid, (long)status, 0, 0, 0, 0);
}
long sys_waitpid_nb(int pid, int *status)
{
    return syscall(SYS_WAITPID_NB, (long)pid, (long)status, 0, 0, 0, 0);
}
int sys_ps(void *buf, int max)
{
    return (int)syscall(SYS_PS, (long)buf, (long)max, 0, 0, 0, 0);
}
void sys_exit(int code) { syscall(SYS_EXIT, (long)code, 0, 0, 0, 0, 0); }
void sys_reboot(void) { syscall(SYS_REBOOT, 0, 0, 0, 0, 0, 0); }
void sys_poweroff(void) { syscall(SYS_POWEROFF, 0, 0, 0, 0, 0, 0); }
int sys_getpwnam(const char *name, uid_t *uid, gid_t *gid)
{
    return (int)syscall(SYS_GETPWNAM, (long)name, (long)uid, (long)gid, 0, 0, 0);
}

int sys_reload_users(void)
{
    return (int)syscall(SYS_RELOAD_USERS, 0, 0, 0, 0, 0, 0);
}

int ipc_create(void)
{
    return (int)syscall(SYS_IPC_CREATE, 0, 0, 0, 0, 0, 0);
}
long ipc_send(int chan, const void *buf, size_t len)
{
    return syscall(SYS_IPC_SEND, (long)chan, (long)buf, (long)len, 0, 0, 0);
}
long ipc_recv(int chan, void *buf, size_t max)
{
    return syscall(SYS_IPC_RECV, (long)chan, (long)buf, (long)max, 0, 0, 0);
}

long shm_create(size_t bytes)
{
    return syscall(SYS_SHM_CREATE, (long)bytes, 0, 0, 0, 0, 0);
}
void *shm_attach(long id)
{
    return (void *)syscall(SYS_SHM_ATTACH, id, 0, 0, 0, 0, 0);
}

int sock_create(int domain, int type, int proto)
{
    return (int)syscall(SYS_SOCKET_CREATE, (long)domain, (long)type,
                        (long)proto, 0, 0, 0);
}

int sock_bind(int fd, const struct sockaddr *addr, int addrlen)
{
    return (int)syscall(SYS_SOCKET_BIND, (long)fd, (long)addr,
                        (long)addrlen, 0, 0, 0);
}

int sock_connect(int fd, const struct sockaddr *addr, int addrlen)
{
    return (int)syscall(SYS_SOCKET_CONNECT, (long)fd, (long)addr,
                        (long)addrlen, 0, 0, 0);
}

int sock_listen(int fd)
{
    return (int)syscall(SYS_SOCKET_LISTEN, (long)fd, 0, 0, 0, 0, 0);
}

int sock_accept(int fd)
{
    return (int)syscall(SYS_SOCKET_ACCEPT, (long)fd, 0, 0, 0, 0, 0);
}

long sock_send(int fd, const void *buf, size_t len)
{
    return syscall(SYS_SOCKET_SEND, (long)fd, (long)buf, (long)len, 0, 0, 0);
}

long sock_recv(int fd, void *buf, size_t max)
{
    return syscall(SYS_SOCKET_RECV, (long)fd, (long)buf, (long)max, 0, 0, 0);
}

int sock_close(int fd)
{
    return (int)syscall(SYS_SOCKET_CLOSE, (long)fd, 0, 0, 0, 0, 0);
}

int dns_resolve(const char *name, uint32_t *ip_out)
{
    return (int)syscall(SYS_DNS_RESOLVE, (long)name, (long)ip_out, 0, 0, 0, 0);
}

int ping(uint32_t dst_be32, uint32_t timeout_ms, uint32_t *rtt_us)
{
    return (int)syscall(SYS_PING, (long)dst_be32, (long)timeout_ms,
                        (long)rtt_us, 0, 0, 0);
}

uid_t getuid(void)  { return (uid_t)syscall(SYS_GETUID, 0,0,0,0,0,0); }
uid_t geteuid(void) { return (uid_t)syscall(SYS_GETEUID, 0,0,0,0,0,0); }
gid_t getgid(void)  { return (gid_t)syscall(SYS_GETGID, 0,0,0,0,0,0); }
gid_t getegid(void) { return (gid_t)syscall(SYS_GETEGID, 0,0,0,0,0,0); }
int  getrole(void)  { return (int)syscall(SYS_GETROLE, 0,0,0,0,0,0); }
unsigned long long getcap(void) { return (unsigned long long)syscall(SYS_GETCAP, 0,0,0,0,0,0); }
int  setuid(uid_t uid) { return (int)syscall(SYS_SETUID, (long)uid, 0,0,0,0,0); }
int  setgid(gid_t gid) { return (int)syscall(SYS_SETGID, (long)gid, 0,0,0,0,0); }
int  chmod(const char *path, unsigned int mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, (long)mode, 0,0,0,0);
}
int  chown(const char *path, uid_t uid, gid_t gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, (long)uid, (long)gid, 0,0,0);
}
int  sys_authenticate(const char *user, const char *pass)
{
    return (int)syscall(SYS_AUTHENTICATE, (long)user, (long)pass, 0,0,0,0);
}

long fb_mmap(int fd, uint64_t off, void *virt, size_t len)
{
    return syscall(SYS_MMAP, (long)fd, (long)off, (long)virt, (long)len, 0, 0);
}

/* ---- loadable kernel modules (.kxt) ---- */
long kxtload(const char *path)
{
    return syscall(SYS_MODULE_LOAD, (long)path, 0, 0, 0, 0, 0);
}
long kxtunload(const char *name)
{
    return syscall(SYS_MODULE_UNLOAD, (long)name, 0, 0, 0, 0, 0);
}

int uname(struct utsname *buf)
{
    return (int)syscall(SYS_UNAME, (long)buf, 0, 0, 0, 0, 0);
}

long lseek(int fd, long offset, int whence)
{
    return syscall(SYS_LSEEK, (long)fd, offset, (long)whence, 0, 0, 0);
}

long pread(int fd, void *buf, size_t len, long off)
{
    return syscall(SYS_PREAD, (long)fd, (long)buf, (long)len, off, 0, 0);
}

long pwrite(int fd, const void *buf, size_t len, long off)
{
    return syscall(SYS_PWRITE, (long)fd, (long)buf, (long)len, off, 0, 0);
}
