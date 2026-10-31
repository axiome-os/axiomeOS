/*
 * net_test.c — userspace networking self-test
 *
 * Exercises, over the real network stack:
 *   1. Loopback TCP end-to-end (fork a client that talks to our own listen
 *      socket on 127.0.0.1:8080 — the whole path via the lo device).
 *   2. Dotted-quad fast-path DNS resolution + a real query to the DHCP/fallback
 *      resolver (10.0.2.3 slirp built-in).
 *   3. ICMP echo request to 127.0.0.1 and to the slirp host alias 10.0.2.2.
 *   4. TCP HTTP GET to 10.0.2.2:8080 (host python http.server).
 */

#include "../libc/syscall.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include <stddef.h>

static unsigned short htons(unsigned short v) { return (v >> 8) | (v << 8); }
static unsigned int htonl(unsigned int v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

static const char *ip4_str(unsigned int ip, char *buf)
{
    buf[0] = 0;
    for (int i = 0; i < 4; i++)
    {
        char o[4];
        unsigned v = (ip >> (i * 8)) & 0xFF;
        o[0] = 0;
        sprintf(o, "%u", v);
        strcat(buf, o);
        if (i < 3) strcat(buf, ".");
    }
    return buf;
}

static void fill_sin(struct sockaddr_in *a, unsigned int ip, unsigned short port)
{
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr = ip;
    for (int i = 0; i < 8; i++) a->sin_zero[i] = 0;
}

static int test_loopback(void)
{
    printf("  [1] Loopback TCP self-connect (127.0.0.1:8080)...\n");

    int srv = sock_create(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("      FAIL: socket_create\n"); return -1; }

    struct sockaddr_in a;
    fill_sin(&a, htonl(0x7F000001), 8080);
    if (sock_bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        printf("      FAIL: bind\n");
        sock_close(srv);
        return -1;
    }
    if (sock_listen(srv) != 0)
    {
        printf("      FAIL: listen\n");
        sock_close(srv);
        return -1;
    }

    long pid = sys_fork();
    if (pid == 0)
    {
        /* --- client child --- */
        int c = sock_create(AF_INET, SOCK_STREAM, 0);
        if (c < 0) { printf("      child: socket_create FAIL\n"); sys_exit(1); }

        struct sockaddr_in ca;
        fill_sin(&ca, htonl(0x7F000001), 8080);
        if (sock_connect(c, (struct sockaddr *)&ca, sizeof(ca)) < 0)
        {
            printf("      child: connect FAIL\n");
            sock_close(c);
            sys_exit(1);
        }
        printf("      child: connected\n");

        const char *payload = "hello from child over loopback";
        long sn = sock_send(c, payload, strlen(payload));
        if (sn != (long)strlen(payload))
        {
            printf("      child: send FAIL (%ld)\n", sn);
            sock_close(c);
            sys_exit(1);
        }
        char echo[128];
        long rn = sock_recv(c, echo, sizeof(echo));
        if (rn >= 0) echo[rn < (long)sizeof(echo) ? rn : (long)sizeof(echo) - 1] = 0;
        int ok = rn == (long)strlen(payload) && strcmp(echo, payload) == 0;
        printf("      child: echo %s (%ld bytes)\n", ok ? "OK" : "MISMATCH", rn);
        sock_close(c);
        sys_exit(ok ? 0 : 1);
    }

    /* --- server parent --- */
    printf("      parent: accepting...\n");
    int cfd = sock_accept(srv);
    if (cfd < 0)
    {
        printf("      FAIL: accept\n");
        sock_close(srv);
        sys_waitpid(pid, 0);
        return -1;
    }
    char buf[256];
    long n = sock_recv(cfd, buf, sizeof(buf));
    if (n < 0) n = 0;
    if (n > 0) sock_send(cfd, buf, (size_t)n);
    printf("      parent: echoed %ld bytes\n", n);
    sock_close(cfd);
    sock_close(srv);

    int status = 0;
    sys_waitpid(pid, &status);
    int ok = (status == 0) && (n > 0);
    printf("      loopback TCP %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

static int test_dns(void)
{
    printf("  [2] DNS resolve...\n");

    unsigned int ip = 0;
    int r = dns_resolve("10.0.2.15", &ip);
    char buf[16];
    if (r != 0)
    {
        printf("      FAIL: literal fast path\n");
        return -1;
    }
    printf("      literal '10.0.2.15' -> %s (PASS)\n", ip4_str(ip, buf));

    /* Real query via the kernel resolver (DHCP server or 10.0.2.3 fallback).
       Without upstream connectivity this may legitimately fail; report
       whatever the resolver answers. */
    ip = 0;
    r = dns_resolve("localhost", &ip);
    if (r == 0)
        printf("      query 'localhost' -> %s\n", ip4_str(ip, buf));
    else
        printf("      query 'localhost' -> unresolved (SKIP if no upstream DNS)\n");
    return 0;
}

static int test_ping(void)
{
    printf("  [3] ICMP echo...\n");

    struct { unsigned int ip; const char *name; int expected; } hosts[] = {
        { htonl(0x7F000001), "127.0.0.1 (loopback)", 1 },
        { htonl(0x0A000202), "10.0.2.2 (slirp host alias)", 1 },
    };
    int rc = 0;
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
    {
        unsigned int rtt = 0;
        int r = ping(hosts[i].ip, 3000, &rtt);
        if (r == 0)
            printf("      %s: reply, rtt %u us (PASS)\n", hosts[i].name, rtt);
        else
        {
            printf("      %s: no reply\n", hosts[i].name);
            if (hosts[i].expected)
            {
                printf("      FAIL\n");
                rc = -1;
            }
        }
    }
    return rc;
}

static int test_http(void)
{
    printf("  [4] TCP HTTP GET to 10.0.2.2:8080...\n");

    int fd = sock_create(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("      FAIL: socket_create\n"); return -1; }

    struct sockaddr_in a;
    fill_sin(&a, htonl(0x0A000202), 8080);
    int r = sock_connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r < 0)
    {
        printf("      FAIL: connect (is 'python3 -m http.server 8080' running?)\n");
        sock_close(fd);
        return -1;
    }
    printf("      connected\n");

    const char *req = "GET / HTTP/1.0\r\n\r\n";
    if (sock_send(fd, req, strlen(req)) != (long)strlen(req))
    {
        printf("      FAIL: send\n");
        sock_close(fd);
        return -1;
    }

    char buf[2048];
    long total = 0;
    for (int attempt = 0; attempt < 200 && total < (long)sizeof(buf); attempt++)
    {
        long n = sock_recv(fd, buf + total, (size_t)(sizeof(buf) - total));
        if (n <= 0) break;
        total += n;
    }

    if (total > 0)
    {
        if (total >= (long)sizeof(buf))
            total = (long)sizeof(buf) - 1;
        buf[total] = '\0';
        long plen = total < 200 ? total : 200;
        printf("      got %ld bytes, start:\n", total);
        printf("      ");
        write(1, buf, (size_t)plen);
        printf("\n      HTTP %s\n",
               (strstr(buf, "HTTP/1.0 200") || strstr(buf, "HTTP/1.1 200"))
                   ? "PASS" : "(non-200 status)");
    }
    else
    {
        printf("      FAIL: no response\n");
        sock_close(fd);
        return -1;
    }

    sock_close(fd);
    return 0;
}

int main(void)
{
    printf("\n=== net_test ===\n");
    int rc = 0;
    rc |= test_loopback();
    rc |= test_dns();
    rc |= test_ping();
    rc |= test_http();
    printf("=== net_test %s ===\n", rc == 0 ? "PASS" : "FAILED");
    sys_exit(rc == 0 ? 0 : 1);
    return 0;
}