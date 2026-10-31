#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "syscall.h"
#include "signal.h"

volatile int got_signal = 0;
long g_parent_pid = 0;

static void handler(int sig)
{
    got_signal = sig;
}

int main(void)
{
    printf("ipc_test: starting\n");

    /* ---- IPC channels: two processes communicate ---- */
    int chan = ipc_create();
    if (chan < 0)
    {
        printf("IPC FAIL: ipc_create\n");
    }
    else
    {
        long pid = sys_fork();
        if (pid == 0)
        {
            char buf[64];
            long n = ipc_recv(chan, buf, sizeof(buf));
            if (n < 0) n = 0;
            buf[n] = 0;
            printf("IPC child recv: '%s' (%ld bytes)\n", buf, n);
            sys_exit(0);
        }
        else
        {
            const char *msg = "hello-ipc";
            long w = ipc_send(chan, msg, (size_t)strlen(msg));
            printf("IPC parent sent %ld bytes to pid %ld\n", w, pid);
            sys_waitpid((int)pid, 0);
        }
    }

    /* ---- Signals: self-directed delivery (save/restore + sigreturn) ---- */
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    sigaction(SIGUSR1, &sa, 0);
    got_signal = 0;
    kill(sys_getpid(), SIGUSR1);
    sys_yield();   /* signal is delivered on return from this syscall */
    if (got_signal == SIGUSR1)
        printf("SIGNAL self: handled sig=%d OK\n", got_signal);
    else
        printf("SIGNAL FAIL: got=%d expected %d\n", got_signal, SIGUSR1);

    /* ---- Signals: child signals parent (cross-process) ---- */
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    sigaction(SIGUSR2, &sa, 0);
    g_parent_pid = sys_getpid();
    got_signal = 0;
    long pid2 = sys_fork();
    if (pid2 == 0)
    {
        kill(g_parent_pid, SIGUSR2);
        sys_exit(0);
    }
    else
    {
        while (!got_signal)
            sys_yield();
        if (got_signal == SIGUSR2)
            printf("SIGNAL xproc: handled sig=%d OK\n", got_signal);
        else
            printf("SIGNAL xproc FAIL: got=%d expected %d\n", got_signal, SIGUSR2);
        sys_waitpid((int)pid2, 0);
    }

    /* ---- Shared memory: parent writes, child reads (mapped in both) ---- */
    long shid = shm_create(4096);
    if (shid < 0)
    {
        printf("SHM FAIL: shm_create\n");
    }
    else
    {
        char *va = (char *)shm_attach(shid);
        if (!va)
        {
            printf("SHM FAIL: shm_attach\n");
        }
        else
        {
            const char *s = "shared-memory-ok";
            memcpy(va, s, strlen(s) + 1);
            long pid3 = sys_fork();
            if (pid3 == 0)
            {
                printf("SHM child sees: '%s'\n", va);
                sys_exit(0);
            }
            else
            {
                printf("SHM parent wrote: '%s'\n", va);
                sys_waitpid((int)pid3, 0);
            }
        }
    }

    printf("ipc_test: done\n");
    sys_exit(0);
    return 0;
}
