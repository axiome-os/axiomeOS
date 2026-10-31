#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

/* Read a line from stdin (fd 0) into buf. */
static int read_line(char *buf, int max)
{
    int i = 0;
    for (;;)
    {
        int c = getchar();
        if (c < 0) { syscall(SYS_YIELD, 0, 0, 0, 0, 0, 0); continue; }
        if (c == '\n' || c == '\r') break;
        if (i < max - 1) buf[i++] = (char)c;
    }
    buf[i] = 0;
    return i;
}

/* Authenticate `user`/`pass` against the kernel user database (issue #32:
   previously login parsed /etc/passwd itself and ignored the password; now
   it delegates to the kernel's security_authenticate, which checks the
   SHA-256 hash). Returns the uid on success, -1 on failure. */
static int authenticate(const char *user, const char *pass)
{
    return sys_authenticate(user, pass);
}

int main(void)
{
    char user[32];
    char pass[64];

    printf("Username: ");
    read_line(user, sizeof(user));
    printf("Password: ");
    read_line(pass, sizeof(pass));

    int uid = authenticate(user, pass);
    if (uid < 0)
    {
        printf("Login failed\n");
        return 1;
    }

    setuid((uid_t)uid);
    setgid((gid_t)uid);
    printf("Logged in as %s (uid %d)\n", user, uid);

    long pid = sys_spawn_cmd("sh", 2);
    if (pid < 0)
    {
        printf("login: failed to start shell\n");
        return 1;
    }
    int status = 0;
    sys_waitpid((int)pid, &status);
    return 0;
}
