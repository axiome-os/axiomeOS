#ifndef AXIOME_LIBC_SIGNAL_H
#define AXIOME_LIBC_SIGNAL_H

#include <stdint.h>
#include <stddef.h>

#define NSIG 32

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SA_RESTART   0x01
#define SA_NOCLDSTOP 0x02
#define SA_SIGINFO   0x04
#define SA_RESTORER  0x08

struct sigaction {
    void (*sa_handler)(int);
    uint64_t sa_flags;
    void (*sa_restorer)(void);
    uint64_t sa_mask;
};

struct sigframe {
    uint64_t rax, rdi, rsi, rdx, r10, r8, r9, rbx, rbp, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, rsp, ss;
    uint64_t oldmask;
};

void __sigreturn_trampoline(void);
int sigaction(int signo, const struct sigaction *act, struct sigaction *oldact);
int kill(int pid, int sig);
void sigreturn(void);

#endif
