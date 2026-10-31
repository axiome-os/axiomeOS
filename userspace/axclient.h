/* axclient.h — one shared client runtime for every guixd window client
   (axterm --wm, axinfo, axlogin, axoobe). Previously each client carried
   its own copy of this glue and the copies drifted (sanitize order, redraw
   policy, arg validation, EOF handling), so one app would work while
   another flaked. All clients must use this file for:

   * argv parsing + validation (`prog --wm <shmid> <w> <h> <evfd> [<gen>]
     ...`): digits-only numbers, shmid > 0, 2 < evfd < MAX_FD.
   * fd hygiene FIRST (before attach): drop every inherited descriptor
     except stdio and evfd. This closes the event pipe's write alias, so
     the compositor's close reliably delivers EOF to the event read.
   * SHM attach + geometry caps + generation check (see wm_abi.h).
   * seqlock frame publication (begin -> draw -> commit).
   * shared /etc/passwd helpers (loop-to-EOF reads, role parsing) and
     password scrubbing.

   Returns the first unconsumed argv index (past the optional numeric
   <gen>), so callers parse their own trailing options from there. */

#ifndef AXCLIENT_H
#define AXCLIENT_H

#include "axgui.h"

#define AXCLIENT_MAX_FD 32 /* must match kernel MAX_FD (kernel/vfs.h) */
#define AXCLIENT_PASSWD_MAX 8192

struct axclient {
    struct wm_win_hdr *hdr;
    struct axgui_fb fb; /* draw-only: fd stays -1, compositor owns PRESENT */
    long shmid;
    int evfd;
    unsigned long gen;
    int gen_valid; /* set when the compositor passed <gen> on argv */
};

/* Digits-only parse with overflow cap. 0 on success, -1 on garbage. */
static long axclient_parse_num(const char *s, long *out)
{
    long v = 0;
    if (!s || !*s || !out)
        return -1;
    while (*s)
    {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (long)(*s - '0');
        if (v > 0x7FFFFFFFL)
            return -1;
        s++;
    }
    *out = v;
    return 0;
}

/* Full memory barrier (seqlock publication needs store ordering). */
static void axclient_barrier(void)
{
    __sync_synchronize();
}

static int axclient_init(struct axclient *cx, int argc, char **argv,
                         const char *prog)
{
    long shmid, evfd, gen;
    if (!cx || !prog)
        return -1;
    cx->hdr = 0;
    cx->shmid = -1;
    cx->evfd = -1;
    cx->gen = 0;
    cx->gen_valid = 0;
    if (!(argc >= 6 && strcmp(argv[1], "--wm") == 0))
    {
        printf("%s: runs as a desktop window client only\n", prog);
        return -1;
    }
    if (axclient_parse_num(argv[2], &shmid) < 0 || shmid <= 0)
    {
        printf("%s: bad shmid\n", prog);
        return -1;
    }
    /* argv[3..4] (w/h) are informational: geometry comes from the header. */
    if (axclient_parse_num(argv[5], &evfd) < 0 || evfd <= 2 ||
        evfd >= AXCLIENT_MAX_FD)
    {
        printf("%s: bad evfd\n", prog);
        return -1;
    }
    cx->shmid = shmid;
    cx->evfd = (int)evfd;
    /* Sanitize before anything else so inherited server fds (other windows'
       event pipes, input, DRI) are gone before we can block their EOF. */
    {
        int fd;
        for (fd = 0; fd < AXCLIENT_MAX_FD; fd++)
        {
            if (fd == 0 || fd == 1 || fd == 2 || fd == cx->evfd)
                continue;
            close(fd);
        }
    }
    cx->hdr = axgui_win_attach(shmid, &cx->fb);
    if (!cx->hdr)
    {
        printf("%s: attach failed\n", prog);
        return -1;
    }
    /* axgui_win_attach already caps geometry; re-check explicitly so a
       corrupt header can never size our framebuffer. */
    if (cx->hdr->w == 0 || cx->hdr->h == 0 || cx->hdr->w > WM_WIN_W ||
        cx->hdr->h > WM_WIN_H)
    {
        printf("%s: bad window geometry\n", prog);
        return -1;
    }
    if (argc > 6 && axclient_parse_num(argv[6], &gen) == 0)
    {
        cx->gen = (unsigned long)gen;
        cx->gen_valid = 1;
        if (cx->hdr->gen != cx->gen)
        {
            /* Stale mapping from a previous occupant: exit instead of
               drawing into someone else's window. */
            printf("%s: generation mismatch\n", prog);
            return -1;
        }
        return 7;
    }
    return 6;
}

/* Frame publication (seqlock): begin (odd) -> draw cx->fb pixels ->
   [hdr->ready = 1 before the first commit] -> commit (even). */
static void axclient_begin(struct axclient *cx)
{
    cx->hdr->seq++;
    axclient_barrier();
}

static void axclient_commit(struct axclient *cx)
{
    axclient_barrier();
    cx->hdr->seq++;
}

/* Caps from compositor (GUI asks guixd which queries gfx). */
static inline uint64_t axclient_caps(struct axclient *cx)
{
    return cx && cx->hdr ? cx->hdr->gfx_caps : 0;
}
static inline int axclient_has_cap(struct axclient *cx, uint64_t cap)
{
    return cx && cx->hdr && (cx->hdr->gfx_caps & cap);
}
static inline const char *axclient_detail_mode(struct axclient *cx)
{
    if (!cx || !cx->hdr) return "simplified";
    return cx->hdr->gfx_detail ? "detailed" : "simplified";
}

/* Compositor asked for exit (it also closes evfd, so recv gets EOF). */
static int axclient_closed(struct axclient *cx)
{
    return cx->hdr->closed != 0;
}

/* Slot was reused under us (only meaningful with <gen> on argv): exit at
   once instead of scribbling on the new occupant's window. */
static int axclient_stale(struct axclient *cx)
{
    return cx->gen_valid && cx->hdr->gen != cx->gen;
}

/* Read a whole (small) file with looped reads; NUL-terminated, truncated
   at cap-1. 0 on success (even if truncated), -1 when not opened. */
static int __attribute__((unused))
axclient_read_whole(const char *path, char *buf, size_t cap, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    size_t got = 0;
    long r;
    if (fd < 0 || !buf || cap == 0)
        return -1;
    while (got + 1 < cap)
    {
        r = read(fd, buf + got, cap - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    if (len_out)
        *len_out = got;
    return 0;
}

/* True when /etc/passwd holds a "user" or "admin" account (passwd format
   name:hash:uid:gid:role:dir:shell). */
static int __attribute__((unused)) axclient_has_regular_user(void)
{
    char text[AXCLIENT_PASSWD_MAX + 1];
    char *line;
    if (axclient_read_whole("/etc/passwd", text, sizeof(text), 0) < 0)
        return 0;
    line = text;
    while (*line)
    {
        char *nl = line;
        char *f[7];
        int nf = 0;
        char *p = line;
        while (*nl && *nl != '\n')
            nl++;
        if (*nl)
            *nl++ = 0;
        while (*p && nf < 7)
        {
            f[nf++] = p;
            while (*p && *p != ':')
                p++;
            if (*p)
                *p++ = 0;
        }
        if (nf >= 7 && f[4][0] &&
            (strcmp(f[4], "user") == 0 || strcmp(f[4], "admin") == 0))
            return 1;
        line = nl;
    }
    return 0;
}

/* Scrub a credential buffer (call after authenticate/hash, on every path). */
static void __attribute__((unused)) axclient_clear_secret(char *p, size_t n)
{
    if (p)
        memset(p, 0, n);
}

#endif
