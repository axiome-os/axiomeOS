/* CPU-bound loop that performs NO syscalls (issue #30). Used to verify that
   SIGINT/SIGKILL delivered from the timer IRQ can interrupt it, since a
   signal delivered only at syscall entry/return can never fire here. */
static volatile unsigned long sink;

int main(void)
{
    volatile unsigned long x = 0;
    for (;;)
        x = x + 1;
    sink = x;
    return 0;
}
