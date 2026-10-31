#include <stdio.h>
#include "syscall.h"

int main(void)
{
    printf("forkt: before fork\n");
    long pid = sys_fork();
    if (pid < 0)
    {
        printf("forkt: fork failed (%ld)\n", pid);
        return 1;
    }
    if (pid == 0)
    {
        printf("forkt: child (pid=%ld)\n", sys_getpid());
        sys_exit(0);
    }
    sys_waitpid((int)pid, 0);
    printf("forkt: parent done\n");
    return 0;
}
