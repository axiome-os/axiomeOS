#ifndef AXIOME_SYSCALL_NUMBERS_H
#define AXIOME_SYSCALL_NUMBERS_H

/*
 * Single source of truth for syscall numbers.
 *
 * Both the kernel (kernel/syscall.h via the table in syscall.c) and the
 * userspace libc (kernel/userspace/libc/syscall.h) must agree on these
 * numbers.  They were previously copy-pasted into two headers with no build
 * time check, so a mismatch silently corrupted the ABI.  Now both headers
 * include THIS file, and a pointer-size static assertion in syscall.c links
 * the dispatch table to the numbers so any drift fails the build.
 *
 * Keep this number-stable: appending is fine, reordering/renumbering breaks
 * every already-built userspace program.
 */

#define SYS_PRINT   0
#define SYS_YIELD   1
#define SYS_EXIT    2
#define SYS_FORK    3

#define SYS_GETPID  5
#define SYS_WAITPID 6
#define SYS_WRITE   7
#define SYS_READ    8
#define SYS_SPAWN_CMD 9
#define SYS_PS     10
#define SYS_OPEN   11
#define SYS_CLOSE  12
#define SYS_MKDIR  13
#define SYS_UNLINK 14
#define SYS_READDIR 15
#define SYS_CHDIR  16
#define SYS_GETCWD 17
#define SYS_FSTAT  18
#define SYS_DUP2   19
#define SYS_PIPE   20
#define SYS_MOUNT  21
#define SYS_UMOUNT 22
#define SYS_KILL   23
#define SYS_SIGACTION 24
#define SYS_SIGRETURN 25
#define SYS_IPC_CREATE 26
#define SYS_IPC_SEND 27
#define SYS_IPC_RECV 28
#define SYS_SHM_CREATE 29
#define SYS_SHM_ATTACH 30
#define SYS_MKFIFO 31
#define SYS_DRIVER_RESCAN 32
#define SYS_SOCKET_CREATE 33
#define SYS_SOCKET_BIND   34
#define SYS_SOCKET_CONNECT 35
#define SYS_SOCKET_SEND   36
#define SYS_SOCKET_RECV   37
#define SYS_SOCKET_CLOSE  38
#define SYS_SOCKET_LISTEN 39
#define SYS_SOCKET_ACCEPT 40

/* ---- user rank system ---- */
#define SYS_GETUID   41
#define SYS_GETEUID  42
#define SYS_GETGID   43
#define SYS_GETEGID  44
#define SYS_SETUID   45
#define SYS_SETGID   46
#define SYS_GETROLE  47
#define SYS_CHMOD    48
#define SYS_CHOWN    49
#define SYS_GETCAP   50
#define SYS_SETCAP   51
#define SYS_GETPWNAM 52

/* ---- loadable kernel modules (.kxt) ---- */
#define SYS_MODULE_LOAD   53
#define SYS_MODULE_UNLOAD 54
#define SYS_MMAP          55

/* ---- system info ---- */
#define SYS_UNAME         56
#define SYS_RELOAD_USERS  57
#define SYS_EXECVE        58

/* ---- time ---- */
#define SYS_TIME          59
#define SYS_GETTIMEOFDAY  60
#define SYS_NANOSLEEP     61

/* ---- file positioning / positional I/O (issue #28) ---- */
#define SYS_LSEEK         62
#define SYS_PREAD         63
#define SYS_PWRITE        64

/* ---- path-based stat (issue #28: stat() crashed on directories) ---- */
#define SYS_STAT          65

/* ---- remove an (empty) directory (issue #31: rm -r) ---- */
#define SYS_RMDIR         66

/* ---- verify a username/password pair via the kernel (issue #32) ---- */
#define SYS_AUTHENTICATE  67

/* ---- networking services (Phase 13) ---- */
#define SYS_DNS_RESOLVE   68   /* dns_resolve(name, ip_out) */
#define SYS_PING          69   /* ping(dst_be32, timeout_ms, *rtt_us) */

/* ---- non-blocking child reap for compositors (axwm polls this per frame)
   instead of stalling on a blocking waitpid) ---- */
#define SYS_WAITPID_NB    70   /* waitpid_nb(pid, *status): pid, -ECHILD, -EAGAIN */

#define __SYS_LAST        71   /* one past the highest number */

#endif
