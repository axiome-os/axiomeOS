#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

/* kxtload — insmod-equivalent for axiomeOS .kxt modules.
 *
 *   kxtload e1000           -> loads /System/Extensions/e1000.kxt
 *   kxtload /path/to/x.kxt  -> loads the given path
 */
static int has_slash(const char *s)
{
    for (; *s; s++)
        if (*s == '/')
            return 1;
    return 0;
}

static void append(char *dst, size_t *len, const char *src)
{
    while (*src)
        dst[(*len)++] = *src++;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: kxtload <name|/path/to/module.kxt>\n");
        return 1;
    }

    char path[256];
    size_t l = 0;
    const char *a = argv[1];
    if (has_slash(a))
        append(path, &l, a);
    else
    {
        append(path, &l, "/System/Extensions/");
        append(path, &l, a);
        if (l < 4 || strcmp(path + l - 4, ".kxt") != 0)
            append(path, &l, ".kxt");
    }
    path[l] = 0;

    long r = kxtload(path);
    if (r < 0)
    {
        printf("kxtload: failed to load '%s' (err %ld)\n", path, r);
        return 1;
    }
    printf("kxtload: loaded '%s'\n", path);
    return 0;
}
