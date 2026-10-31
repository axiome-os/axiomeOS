/*
 * ping.c — send ICMP echo requests and report round-trip times.
 *
 *   ping <dotted-quad>
 */

#include "../libc/syscall.h"
#include "../libc/stdio.h"
#include "../libc/string.h"

static unsigned int htonl(unsigned int v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

static int parse_ip4(const char *s, unsigned int *out)
{
    unsigned parts[4] = { 0, 0, 0, 0 };
    int p = 0;
    unsigned v = 0;
    int any = 0;
    for (const char *c = s; *c; c++)
    {
        if (*c >= '0' && *c <= '9')
        {
            v = v * 10 + (unsigned)(*c - '0');
            if (v > 255) return -1;
            any = 1;
        }
        else if (*c == '.')
        {
            if (!any || p >= 3) return -1;
            parts[p++] = v;
            v = 0;
            any = 0;
        }
        else
        {
            return -1;
        }
    }
    if (!any || p != 3) return -1;
    parts[p] = v;

    unsigned int ip = parts[0] | (parts[1] << 8) | (parts[2] << 16) |
                      (parts[3] << 24);
    (void)htonl;
    *out = ip;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: ping <a.b.c.d>\n");
        sys_exit(1);
    }

    unsigned int ip = 0;
    if (parse_ip4(argv[1], &ip) != 0)
    {
        printf("ping: bad address '%s'\n", argv[1]);
        sys_exit(1);
    }

    int hits = 0;
    for (int i = 0; i < 1; i++)
    {
        unsigned int rtt = 0;
        int r = ping(ip, 3000, &rtt);
        if (r == 0)
        {
            printf("ping %s: reply, rtt %u us\n", argv[1], rtt);
            hits++;
        }
        else
        {
            printf("ping %s: no reply\n", argv[1]);
        }
    }

    sys_exit(hits > 0 ? 0 : 1);
    return 0;
}