/* Host-side stubs for the freestanding libc's kernel boundary.

   Compile this alongside the userspace libc sources when running host unit
   tests.  None of the tested libc code paths actually need real syscalls, so
   these just keep the linker happy and make failure modes explicit. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "errno.h"
#include "syscall.h"

int errno = 0;

long syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)n;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    errno = ENOSYS;
    return -1;
}

/* Forward to the real host fd using a raw syscall so we don't re-enter the
   interposed libc `write` symbol (glibc's stdio would call back into us). */
long write(int fd, const void *buf, size_t len)
{
#if defined(__x86_64__)
    long ret;
#if defined(__APPLE__)
    /* Darwin encodes the syscall class in the high bits: write is
       0x2000000 | 4. A bare 1 is exit(2), which kills the test process. */
    register long r0 __asm__("rax") = 0x2000004; /* SYS_write */
#else
    register long r0 __asm__("rax") = 1; /* SYS_write */
#endif
    register long r1 __asm__("rdi") = fd;
    register long r2 __asm__("rsi") = (long)buf;
    register long r3 __asm__("rdx") = (long)len;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "r"(r0), "r"(r1), "r"(r2), "r"(r3)
                     : "rcx", "r11", "memory");
    return ret;
#else
    (void)fd;
    (void)buf;
    return (long)len;
#endif
}

long read(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    return (long)len;
}

int open(const char *path, int flags)
{
    (void)path;
    (void)flags;
    return -1;
}

int close(int fd)
{
    (void)fd;
    return 0;
}
