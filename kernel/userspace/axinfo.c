/* axinfo — system info window (axiomeOS /Binaries/axinfo)
 * Rebuilt on axmui: the de-facto native HTML+CSS UI toolkit.
 * Runs only as a guixd client: `axinfo --wm <shmid> <w> <h> <evfd> ...`.
 * Declarative HTML+CSS describes the chrome; native raster draws it.
 * Any key, button click or compositor close exits.
 */
#include "axmui/axmui.h"

static axmui_app_t *g_app;

static void on_close(axmui_view_t *v, void *ud){
    (void)v; (void)ud;
    if(g_app) g_app->running=0;
}
static void on_any_key(axmui_view_t *v, void *ud){
    (void)v; (void)ud;
    if(g_app) g_app->running=0;
}

int main(int argc, char **argv){
    struct utsname u;
    char sysline[96]="";
    char machline[64]="";
    char clockline[40]="";
    char winline[48]="";
    long now;
    int hh,mm,ss;

    if(uname(&u)==0){
        snprintf(sysline,sizeof(sysline),"kernel: %s %s", u.sysname, u.release);
        snprintf(machline,sizeof(machline),"machine: %s", u.machine);
    } else {
        snprintf(sysline,sizeof(sysline),"kernel: axiomeOS");
        snprintf(machline,sizeof(machline),"machine: x86_64");
    }
    now=(long)time(0); if(now<0) now=0;
    ss=(int)((now%60+60)%60);
    mm=(int)(((now/60)%60+60)%60);
    hh=(int)(((now/3600)%24+24)%24);
    snprintf(clockline,sizeof(clockline),"clock: %02d:%02d:%02d UTC",hh,mm,ss);
    snprintf(winline,sizeof(winline),"window: %ux%u", WM_WIN_W, WM_WIN_H);

    g_app=axmui_app_create();
    if(!g_app) return 1;
    axmui_window_t *win=axmui_window_create(g_app,"System info", WM_WIN_W, WM_WIN_H);
    if(!win) return 1;

    char html[1400];
    snprintf(html,sizeof(html),
        "<div class='col'>"
        "  <div class='header'>"
        "    <h1>axiomeOS - system info</h1>"
        "  </div>"
        "  <div class='card'>"
        "    <div class='row'><span class='muted'>-</span><p>%s</p></div>"
        "    <div class='row'><span class='muted'>-</span><p>%s</p></div>"
        "    <div class='row'><span class='muted'>-</span><p>%s</p></div>"
        "    <div class='row'><span class='muted'>-</span><p>%s</p></div>"
        "  </div>"
        "  <p class='muted' style='text-align:center'>Press any key or click Close to dismiss.</p>"
        "  <div class='row' style='justify-content:center'>"
        "    <button id='close' class='btn-primary'>Close</button>"
        "  </div>"
        "</div>",
        sysline, machline, clockline, winline);

    const char *css =
        ".col{ gap:14px; }"
        ".header{ padding:4px 2px; }"
        ".card{ gap:8px; }"
        "h1{ font-weight:bold; }";

    axmui_window_set_html(win, html, css);
    axmui_view_t *closeBtn=axmui_view_find(axmui_window_root(win),"close");
    if(closeBtn) axmui_view_on_click(closeBtn, on_close, NULL);
    axmui_window_on_key(win, on_any_key, NULL);

    /* focus close button for keyboard Enter */
    win->focused=closeBtn; if(closeBtn) closeBtn->focused=1;

    return axmui_app_run(g_app, argc, argv, "axinfo");
}
