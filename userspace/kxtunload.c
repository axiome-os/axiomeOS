#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

/* kxtunload — rmmod-equivalent for axiomeOS .kxt modules.
 *
 *   kxtunload e1000
 */
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: kxtunload <name>\n");
        return 1;
    }
    long r = kxtunload(argv[1]);
    if (r < 0)
    {
        printf("kxtunload: failed to unload '%s' (err %ld)\n", argv[1], r);
        return 1;
    }
    printf("kxtunload: unloaded '%s'\n", argv[1]);
    return 0;
}
