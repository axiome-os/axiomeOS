/* evtest — input device diagnostic (/Binaries/evtest).
   Polls /Devices/input0 and prints every event to stdout. Takes the GUI
   grab while running so PS/2/USB keystrokes land only here (not in the
   shell underneath); the framebuffer stays quiet under grab, so output is
   observed over the serial console. Usage: evtest [max-events]. */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include <stdint.h>

#include "../input_abi.h"

int main(int argc, char **argv)
{
    int fd;
    int max_ev = 8;
    int got_ev = 0;
    int empty_ms = 0;
    uint32_t grab;

    if (argc > 1)
        max_ev = atoi(argv[1]);
    if (max_ev <= 0 || max_ev > 256)
        max_ev = 8;

    fd = open("/Devices/input0", 0);
    if (fd < 0)
    {
        printf("evtest: cannot open /Devices/input0\n");
        return 1;
    }
    grab = 1;
    write(fd, &grab, sizeof(grab));
    printf("evtest: grab held, waiting for %d events...\n", max_ev);

    while (got_ev < max_ev && empty_ms < 25000)
    {
        struct axinput_event ev[16];
        long r = read(fd, ev, sizeof(ev));
        struct timespec ts;
        if (r > 0)
        {
            int n = (int)((size_t)r / sizeof(ev[0]));
            int i;
            for (i = 0; i < n && got_ev < max_ev; i++)
            {
                if (ev[i].type == AXINPUT_TYPE_KEY)
                    printf("EV key code=%u\n", ev[i].code);
                else if (ev[i].type == AXINPUT_TYPE_MOUSE)
                    printf("EV mouse dx=%d dy=%d btn=%u\n", ev[i].dx,
                           ev[i].dy, ev[i].code);
                else
                    printf("EV unknown type=%u\n", ev[i].type);
                got_ev++;
            }
            empty_ms = 0;
        }
        else
        {
            ts.tv_sec = 0;
            ts.tv_nsec = 10 * 1000000L;
            nanosleep(&ts, 0);
            empty_ms += 10;
        }
    }

    grab = 0;
    write(fd, &grab, sizeof(grab));
    close(fd);
    printf("evtest: done (%d events)\n", got_ev);
    return 0;
}
