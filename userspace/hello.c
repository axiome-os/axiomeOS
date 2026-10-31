#include "stdio.h"
#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Hello from pid %d!\n", (int)sys_getpid());
    sys_exit(0);
    return 0;
}
