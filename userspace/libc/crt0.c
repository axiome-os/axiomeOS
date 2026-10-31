#include "syscall.h"

extern int main(int argc, char **argv, char **envp);
void __axiome_set_environ(char **envp);

__attribute__((naked))
void _start(void)
{
    __asm__ volatile (
        "mov (%%rsp), %%rdi\n\t"
        "lea 8(%%rsp), %%rsi\n\t"
        "lea 8(%%rsi,%%rdi,8), %%rdx\n\t"
        "mov %%rdi, %%r8\n\t"
        "mov %%rsi, %%r9\n\t"
        "mov %%rdx, %%rdi\n\t"
        "call __axiome_set_environ\n\t"
        "mov %%r8, %%rdi\n\t"
        "mov %%r9, %%rsi\n\t"
        "call main\n\t"
        "mov %%eax, %%edi\n\t"
        "mov $%c[sc], %%rax\n\t"
        "syscall\n\t"
        : : [sc] "i" (SYS_EXIT)
        : "rax", "rdi", "rsi", "rdx", "r8", "r9", "rcx", "r11", "memory"
    );
}