/* axinfo — system info window (axiomeOS /Binaries/axinfo)
 * Uses axmui (HTML+CSS UI toolkit) over the axw display server protocol.
 * Connects to /devices/axw0, creates an axw surface + shm buffer,
 * renders system info via axmui, commits to the compositor.
 * Any key closes the window.
 */
#include "axw-protocol.h"
#include "axw-client.h"
#include "axgui.h"
#include "axmui/axmui.h"
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "time.h"
#include <stddef.h>
#include <stdint.h>

#define AXINFO_W 620
#define AXINFO_H 420

static axmui_app_t *g_app = 0;
static axmui_window_t *g_win = 0;
static int g_running = 1;

static void on_close(axmui_view_t *sender, void *ud)
{
    (void)sender; (void)ud;
    g_running = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct utsname u;
    char sysline[96] = "";
    char machline[64] = "";
    char clockline[40] = "";
    long now;
    int hh, mm, ss;

    if (uname(&u) == 0)
    {
        snprintf(sysline, sizeof(sysline), "kernel: %s %s", u.sysname, u.release);
        snprintf(machline, sizeof(machline), "machine: %s", u.machine);
    }
    else
    {
        snprintf(sysline, sizeof(sysline), "kernel: axiomeOS");
        snprintf(machline, sizeof(machline), "machine: x86_64");
    }
    now = (long)time(0);
    if (now < 0) now = 0;
    ss = (int)((now % 60 + 60) % 60);
    mm = (int)(((now / 60) % 60 + 60) % 60);
    hh = (int)(((now / 3600) % 24 + 24) % 24);
    snprintf(clockline, sizeof(clockline), "clock: %02d:%02d:%02d UTC", hh, mm, ss);

    /* Build HTML+CSS UI */
    char html[512];
    snprintf(html, sizeof(html),
        "<div class='window'>"
        "  <h1>axiomeOS - system info</h1>"
        "  <div class='card'>"
        "    <p>%s</p>"
        "    <p>%s</p>"
        "    <p>%s</p>"
        "    <p>window: %dx%d (axw + axmui protocol)</p>"
        "  </div>"
        "  <div class='row'>"
        "    <button id='close'>Close</button>"
        "  </div>"
        "  <p class='muted'>Press any key or click Close to dismiss.</p>"
        "</div>",
        sysline, machline, clockline, AXINFO_W, AXINFO_H);

    const char *css =
        " .window{ padding:20px; gap:16px; }"
        " h1{ color:#E2E8F0; font-weight:bold; font-size:16; }"
        " .card{ padding:16px; gap:10px; }"
        " p{ color:#94A3B8; font-size:13; }"
        " .row{ justify-content:flex-end; gap:8px; }"
        " button#close{ background:#5E81AC; color:white; border-radius:8px; padding:8px 16px; }"
        " button#close:hover{ background:#81A1C1; }";

    g_app = axmui_app_create();
    if (!g_app) return 1;

    g_win = axmui_window_create(g_app, "System Info", AXINFO_W, AXINFO_H);
    if (!g_win) { axmui_app_destroy(g_app); return 1; }

    axmui_window_set_html(g_win, html, css);

    /* Wire close button */
    axmui_view_t *btn = axmui_view_find(axmui_window_root(g_win), "close");
    if (btn) axmui_view_on_click(btn, on_close, NULL);

    /* Any key also closes */
    axmui_window_on_key(g_win, on_close, NULL);

    return axmui_app_run(g_app, argc, argv, "axinfo");
}
