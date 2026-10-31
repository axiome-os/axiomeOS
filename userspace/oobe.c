#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"

/* ===========================================================================
 * axiome-oobe - first-boot "out of the box" experience (CLI variant).
 *
 * Manual/headless use: run /bin/oobe from a console to create the first
 * user. On a graphical boot the desktop owns first-boot setup instead
 * (guixd opens the axoobe window when no regular user exists), so
 * axiome-init no longer spawns this wizard automatically. Either way it
 * stays idempotent: a no-op as soon as a regular user (role "user" or
 * "admin") exists in /etc/passwd.
 *
 * The wizard:
 *   * asks for a username, password and confirmation
 *   * optionally grants the admin role
 *   * creates the user's home directory under /Users
 *   * appends a line to /etc/passwd (sha256 hash, matching kernel security.c)
 *   * asks the kernel to reload its in-kernel user database
 *
 * Console input is echoed by the kernel tty layer (there is no termios
 * "noecho" mode), so the password is visible while typing -- the same as
 * /bin/login. This is a minimal-OS limitation, not a security feature.
 * =========================================================================== */

#define PASSWD_PATH "/etc/passwd"
#define USERS_DIR   "/Users"
#define PASSWD_MAX  4096

/* ---- minimal SHA-256 (matches the kernel verifier in security.c) ---- */

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = len + 1;
    uint64_t bitlen = (uint64_t)len * 8;
    size_t padded = total + (64 - (total % 64)) % 64;
    if ((padded % 64) == 0 && (total % 64) > 56) padded += 64;
    else if (total % 64 == 0) padded = total + 64;

    uint8_t *buf = malloc(padded ? padded : 64);
    if (!buf) { memset(out, 0, 32); return; }
    size_t i;
    for (i = 0; i < len; i++) buf[i] = msg[i];
    buf[i++] = 0x80;
    while (i < padded - 8) buf[i++] = 0;
    for (int j = 0; j < 8; j++)
        buf[padded - 1 - j] = (uint8_t)(bitlen >> (8 * j));

    for (size_t off = 0; off < padded; off += 64)
    {
        uint32_t w[64];
        for (int t = 0; t < 16; t++)
            w[t] = ((uint32_t)buf[off + t*4] << 24) |
                   ((uint32_t)buf[off + t*4+1] << 16) |
                   ((uint32_t)buf[off + t*4+2] << 8) |
                   ((uint32_t)buf[off + t*4+3]);
        for (int t = 16; t < 64; t++)
            w[t] = (rotr(w[t-2],17) ^ rotr(w[t-2],19) ^ (w[t-2]>>10)) + w[t-7] +
                   (rotr(w[t-15],7) ^ rotr(w[t-15],18) ^ (w[t-15]>>3)) + w[t-16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int t = 0; t < 64; t++)
        {
            uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch = (e&f)^((~e)&g);
            uint32_t t1 = hh + S1 + ch + K[t] + w[t];
            uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    free(buf);
    for (int i = 0; i < 8; i++)
    {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

static void sha256_hex(const char *msg, char *hex)
{
    uint8_t d[32];
    sha256((const uint8_t *)msg, strlen(msg), d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
    {
        hex[i*2]   = hx[d[i] >> 4];
        hex[i*2+1] = hx[d[i] & 0xF];
    }
    hex[64] = 0;
}

/* ---- small helpers ---- */

/* Read a line from stdin (fd 0). Keystrokes are echoed by the kernel tty
   layer, so no manual echo is needed here (same pattern as /bin/login).
   Returns the number of chars read. */
static int read_line(char *buf, int max)
{
    int i = 0;
    for (;;)
    {
        int c = getchar();
        if (c < 0) { sys_yield(); continue; }
        if (c == '\n' || c == '\r') break;
        if (c == 127 || c == 8)  /* backspace */
        {
            if (i > 0) i--;
            continue;
        }
        if (c >= 32 && c < 127 && i < max - 1)
            buf[i++] = (char)c;
    }
    buf[i] = 0;
    return i;
}

static char *read_whole(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { *len_out = 0; return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0)
    {
        close(fd);
        *len_out = 0;
        return 0;
    }
    size_t sz = (size_t)st.st_size;
    if (sz > PASSWD_MAX) sz = PASSWD_MAX;
    char *buf = malloc(sz + 1);
    if (!buf) { close(fd); *len_out = 0; return 0; }
    size_t got = 0;
    while (got < sz)
    {
        long r = read(fd, buf + got, sz - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    close(fd);
    *len_out = got;
    return buf;
}

static int write_whole(const char *path, const char *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len)
    {
        long r = write(fd, buf + off, len - off);
        if (r <= 0) { close(fd); return -1; }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

/* A passwd role of "user" or "admin" means a real account already exists. */
static int has_regular_user(void)
{
    size_t len;
    char *text = read_whole(PASSWD_PATH, &len);
    if (!text) return 0;
    char *line = text;
    while (*line)
    {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        char saved = *nl;
        *nl = 0;
        char *f[7];
        int nf = 0;
        char *p = line;
        while (*p && nf < 7)
        {
            f[nf++] = p;
            while (*p && *p != ':') p++;
            if (*p) { *p = 0; p++; }
        }
        if (nf >= 7 && f[4][0])
        {
            if (strcmp(f[4], "user") == 0 || strcmp(f[4], "admin") == 0)
            {
                free(text);
                return 1;
            }
        }
        *nl = saved;
        if (!*nl) break;
        line = nl + 1;
    }
    free(text);
    return 0;
}

static int valid_username(const char *s)
{
    if (s[0] == 0 || strlen(s) > 31)
        return 0;
    while (*s)
    {
        char c = *s++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

/* concat `what` (a literal/name) after dst */
static void cat(char *dst, size_t *pos, size_t cap, const char *what)
{
    while (*what && *pos + 1 < cap)
        dst[(*pos)++] = *what++;
    dst[*pos] = 0;
}

int main(void)
{
    puts("");
    puts("==================================================");
    puts("  axiomeOS first-boot setup");
    puts("==================================================");

    if (has_regular_user())
    {
        puts("A user account already exists -- nothing to set up.");
        puts("Type 'login' to sign in, or 'su' to switch users.");
        return 0;
    }

    char name[32];
    for (;;)
    {
        puts("No user account found. Create one to use this system.");
        printf("Username: ");
        if (read_line(name, sizeof(name)) == 0)
        {
            puts("Username cannot be empty.");
            continue;
        }
        if (!valid_username(name))
        {
            puts("Usernames may only contain letters, digits, '_', '-' and '.',");
            puts("and must be at most 31 chars.");
            continue;
        }
        break;
    }

    char pass[64];
    for (;;)
    {
        printf("Password: ");
        if (read_line(pass, sizeof(pass)) == 0)
        {
            puts("Password cannot be empty.");
            continue;
        }
        if (strlen(pass) < 4)
        {
            puts("Password must be at least 4 characters.");
            continue;
        }
        char confirm[64];
        printf("Confirm password: ");
        read_line(confirm, sizeof(confirm));
        if (strcmp(pass, confirm) != 0)
        {
            puts("Passwords do not match. Try again.");
            continue;
        }
        break;
    }

    printf("Grant %s administrator privileges? [y/N]: ", name);
    char ans[8];
    read_line(ans, sizeof(ans));
    int admin = (ans[0] == 'y' || ans[0] == 'Y');

    /* ---- create the user's home directory ---- */
    char dir[128];
    {
        size_t pos = 0;
        cat(dir, &pos, sizeof(dir), USERS_DIR);
        cat(dir, &pos, sizeof(dir), "/");
        cat(dir, &pos, sizeof(dir), name);
        if (mkdir(dir) != 0)
        {
            /* The directory may already exist (e.g. re-provisioning). */
            struct stat st;
            if (stat(dir, &st) != 0 || !(st.st_mode & S_IFDIR))
            {
                printf("oobe: could not create %s\n", dir);
                return 1;
            }
        }
        chown(dir, 1000, 1000);
    }

    /* ---- append passwd line ---- */
    size_t plen;
    char *old = read_whole(PASSWD_PATH, &plen);
    if (!old)
    {
        puts("oobe: cannot read /etc/passwd");
        return 1;
    }
    if (has_regular_user())
    {
        /* Another run won the race; nothing to do. */
        free(old);
        return 0;
    }

    char hash[65];
    sha256_hex(pass, hash);

    char newbuf[PASSWD_MAX];
    size_t pos = 0;
    newbuf[0] = 0;
    cat(newbuf, &pos, sizeof(newbuf), old);
    if (pos == 0 || newbuf[pos - 1] != '\n')
        cat(newbuf, &pos, sizeof(newbuf), "\n");
    cat(newbuf, &pos, sizeof(newbuf), name);
    cat(newbuf, &pos, sizeof(newbuf), ":");
    cat(newbuf, &pos, sizeof(newbuf), hash);
    cat(newbuf, &pos, sizeof(newbuf), ":1000:1000:");
    cat(newbuf, &pos, sizeof(newbuf), admin ? "admin" : "user");
    cat(newbuf, &pos, sizeof(newbuf), ":");
    cat(newbuf, &pos, sizeof(newbuf), dir);
    cat(newbuf, &pos, sizeof(newbuf), ":/bin/sh\n");

    if (write_whole(PASSWD_PATH, newbuf, pos) != 0)
    {
        puts("oobe: could not update /etc/passwd");
        free(old);
        return 1;
    }
    free(old);

    /* Refresh the kernel's user database so getpwnam/login see the new user. */
    if (sys_reload_users() != 0)
        puts("oobe: warning: could not refresh kernel user database");

    printf("Account %s created%s. You can now log in.\n",
           name, admin ? " (administrator)" : "");
    return 0;
}