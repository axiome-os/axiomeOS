#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"

/* syslogd - a long-running axiome-init service.
 *
 * Demonstrates the "logging" half of the service manager: instead of writing
 * to the inherited console (which would interleave with every other service),
 * it appends structured heartbeat lines to its own log file. axiome-init
 * records the *lifecycle* of the service (start/stop/restart) separately.
 *
 * It never exits on its own; axiome-init is responsible for its lifecycle. */

#define LOG_PATH "/var/log/syslog.log"

static void delay(void)
{
    struct timespec ts;
    ts.tv_sec  = 1;
    ts.tv_nsec = 0;
    nanosleep(&ts, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Log file is created at image-build time; open for append only
       (runtime O_CREAT is currently broken in axiomefs). */
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND);
    if (fd < 0)
    {
        printf("syslogd: cannot open %s\n", LOG_PATH);
        sys_exit(1);
    }

    unsigned long beat = 0;
    for (;;)
    {
        char msg[128];
        int n = 0;
        const char *pre = "syslogd: heartbeat #";
        for (const char *p = pre; *p; p++) msg[n++] = *p;
        /* append the counter (decimal) */
        char num[16];
        int ni = 0;
        unsigned long v = beat;
        if (v == 0) num[ni++] = '0';
        while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
        while (ni--) msg[n++] = num[ni];
        const char *suf = "\n";
        for (const char *p = suf; *p; p++) msg[n++] = *p;

        write(fd, msg, (size_t)n);
        beat++;
        delay();
    }
    /* unreachable */
}
