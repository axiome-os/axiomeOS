#include "syscall.h"
#include "signal.h"
#include "errno.h"
#include <stddef.h>

void __sigreturn_trampoline(void)
{
    register long n __asm__("rax") = SYS_SIGRETURN;
    __asm__ volatile ("syscall" : : "r"(n) : "%rcx", "%r11", "memory");
    for (;;) { }
}

int sigaction(int signo, const struct sigaction *act, struct sigaction *oldact)
{
    struct sigaction local;
    const struct sigaction *p = act;
    if (act)
    {
        local = *act;
        local.sa_restorer = __sigreturn_trampoline;
        local.sa_flags |= SA_RESTORER;
        p = &local;
    }
    return (int)syscall(SYS_SIGACTION, (long)signo, (long)p,
                        (long)oldact, 0, 0, 0);
}

int kill(int pid, int sig)
{
    return (int)syscall(SYS_KILL, (long)pid, (long)sig, 0, 0, 0, 0);
}

void sigreturn(void)
{
    syscall(SYS_SIGRETURN, 0, 0, 0, 0, 0, 0);
}
