#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

/* su: switch the current process to another uid/gid (requires CAP_SETUID)
    and exec a shell. With no argument, switches to uid 0 (admin). A
    non-numeric argument is resolved through the user database. */
int main(int argc, char **argv)
{
    uid_t uid = 0;
    gid_t gid = 0;

    if (argc > 1)
    {
        /* Try to resolve as a username first, then fall back to numeric. */
        if (sys_getpwnam(argv[1], &uid, &gid) != 0)
            uid = (uid_t)atoi(argv[1]);
    }

    if (setuid(uid) < 0)
    {
        printf("su: permission denied (need CAP_SETUID)\n");
        return 1;
    }
    if (setgid(gid) < 0)
    {
        printf("su: failed to set group\n");
        return 1;
    }

    /* Re-exec a shell as the new identity. */
    long pid = sys_spawn_cmd("sh", 2);
    if (pid < 0)
    {
        printf("su: failed to start shell\n");
        return 1;
    }
    int status = 0;
    sys_waitpid((int)pid, &status);
    return 0;
}
