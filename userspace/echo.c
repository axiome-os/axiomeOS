#include "stdio.h"
#include "syscall.h"

/* GNU-style echo:
 *   echo [-neE] [arg...]
 *   -n    do not output the trailing newline
 *   -e    enable interpretation of backslash escapes
 *   -E    disable interpretation of backslash escapes (default)
 * Options are recognized until the first non-option argument, and may be
 * combined (e.g. -ne or -nnee).
 */

static void emit_plain(const char *s)
{
    while (*s)
        putchar((unsigned char)*s++);
}

/* Emit s interpreting backslash escapes. Returns 1 if a \c escape (stop
   output and suppress the trailing newline) was seen. */
static int emit_escaped(const char *s)
{
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (c != '\\')
        {
            putchar(c);
            continue;
        }
        if (!*s)
        {
            putchar('\\');
            break;
        }
        unsigned char e = (unsigned char)*s++;
        switch (e)
        {
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'c': return 1;
        case 'e': putchar(27); break;
        case 'f': putchar('\f'); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
        {
            unsigned int v = (unsigned int)(e - '0');
            int n = 1;
            while (n < 3 && *s >= '0' && *s <= '7')
            {
                v = v * 8 + (unsigned int)(*s - '0');
                s++;
                n++;
            }
            putchar(v & 0xff);
            break;
        }
        case 'x':
        {
            unsigned int v = 0;
            int n = 0;
            while (n < 2 && *s &&
                   ((*s >= '0' && *s <= '9') ||
                    (*s >= 'a' && *s <= 'f') ||
                    (*s >= 'A' && *s <= 'F')))
            {
                unsigned int d;
                if (*s >= '0' && *s <= '9') d = (unsigned int)(*s - '0');
                else if (*s >= 'a' && *s <= 'f') d = 10u + (unsigned int)(*s - 'a');
                else d = 10u + (unsigned int)(*s - 'A');
                v = v * 16 + d;
                s++;
                n++;
            }
            putchar(v & 0xff);
            break;
        }
        default:
            putchar('\\');
            putchar(e);
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int no_nl = 0;
    int interpret = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0)
    {
        const char *p = argv[i] + 1;
        int only_opts = 1;
        for (const char *q = p; *q; q++)
            if (*q != 'e' && *q != 'n' && *q != 'E')
            {
                only_opts = 0;
                break;
            }
        if (!only_opts)
            break;
        for (const char *q = p; *q; q++)
        {
            if (*q == 'n') no_nl = 1;
            else if (*q == 'e') interpret = 1;
            else if (*q == 'E') interpret = 0;
        }
        i++;
    }

    int first = 1;
    int stop = 0;
    for (; i < argc; i++)
    {
        if (!first)
            putchar(' ');
        first = 0;
        if (interpret)
        {
            if (emit_escaped(argv[i]))
            {
                stop = 1;
                break;
            }
        }
        else
        {
            emit_plain(argv[i]);
        }
    }

    if (!no_nl && !stop)
        putchar('\n');
    sys_exit(0);
    return 0;
}