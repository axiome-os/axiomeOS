/* axinfo — system info window client for guixd (/Binaries/axinfo).
   Runs only as a compositor client: `axinfo --wm <shmid> <w> <h> <evfd> ...`.
   Draws uname + clock + help into the window SHM segment; any key (or the
   compositor's close request) exits. All WM glue comes from axclient.h. */

#include "axclient.h"

#define INFO_BG 0x101418u
#define INFO_FG 0xD8DEE9u
#define INFO_HD 0x88C0D0u
#define INFO_DM 0x4C566Au

static void info_draw(struct axgui_fb *fb)
{
    struct utsname u;
    char line[128];
    long now;
    int y = 8;
    int hh, mm, ss;

    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, INFO_BG);
    axgui_text(fb, "axiomeOS - system info", 8, y, INFO_HD, INFO_BG);
    y += FONT_HEIGHT + 4;

    if (uname(&u) == 0)
    {
        snprintf(line, sizeof(line), "kernel: %s %s", u.sysname, u.release);
        axgui_text(fb, line, 8, y, INFO_FG, INFO_BG);
        y += FONT_HEIGHT;
        snprintf(line, sizeof(line), "machine: %s", u.machine);
        axgui_text(fb, line, 8, y, INFO_FG, INFO_BG);
        y += FONT_HEIGHT;
    }
    now = (long)time(0);
    if (now < 0)
        now = 0;
    ss = (int)((now % 60 + 60) % 60);
    mm = (int)(((now / 60) % 60 + 60) % 60);
    hh = (int)(((now / 3600) % 24 + 24) % 24);
    snprintf(line, sizeof(line), "clock: %02d:%02d:%02d UTC", hh, mm, ss);
    axgui_text(fb, line, 8, y, INFO_FG, INFO_BG);
    y += FONT_HEIGHT;

    snprintf(line, sizeof(line), "window: %ux%u shm client", fb->w, fb->h);
    axgui_text(fb, line, 8, y, INFO_FG, INFO_BG);
    y += FONT_HEIGHT + 4;

    axgui_text(fb, "axinfo is a real /Binaries program hosted", 8, y,
               INFO_DM, INFO_BG);
    y += FONT_HEIGHT;
    axgui_text(fb, "by guixd (pixels via SHM, keys via pipe).", 8, y,
               INFO_DM, INFO_BG);
    y += FONT_HEIGHT + 4;

    axgui_text(fb, "press any key to close", 8, y, INFO_HD, INFO_BG);
}

int main(int argc, char **argv)
{
    struct axclient cx;
    if (axclient_init(&cx, argc, argv, "axinfo") < 0)
        return 1;

    axclient_begin(&cx);
    info_draw(&cx.fb);
    cx.hdr->ready = 1;
    axclient_commit(&cx);

    for (;;)
    {
        struct wm_event ev;
        if (axclient_closed(&cx) || axclient_stale(&cx))
            break;
        if (axgui_wm_recv(cx.evfd, &ev) < 0)
            break;
        if (axclient_closed(&cx) || axclient_stale(&cx))
            break;
        if (ev.type == WM_EV_KEY)
            break; /* any key closes */
        /* Mouse motion: refresh the clock line cheaply. */
        axclient_begin(&cx);
        info_draw(&cx.fb);
        axclient_commit(&cx);
    }
    return 0;
}
