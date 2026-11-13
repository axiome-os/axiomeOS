/* axlogin — GUI login (axiomeOS /Binaries/axlogin).
 * Rebuilt on axw + axmui: native HTML+CSS login form.
 * Shows username/password form. On success authenticates and exits 0 so the
 * display server can start the user session. On failure shows an error.
 * The legacy session-file hand-off to guixd is gone (guixd removed); the
 * display server (axdisplay-server) owns the session now. */
#include "axw-protocol.h"
#include "axw-client.h"
#include "axgui.h"
#include "wm_abi.h"
#include "axsha256.h"
#include "axmui/axmui.h"
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stddef.h>
#include <stdint.h>

#define AXLOGIN_RUN_DIR "/var/run"

static axmui_app_t *g_app;
static axmui_window_t *g_win;
static int g_login_ok = 0;

static void login_scrub(axmui_view_t *v){
    if(!v) return;
    memset(v->value,0,sizeof(v->value));
    memset(v->text,0,sizeof(v->text));
}

static void set_status(const char *s){
    axmui_view_t *st=axmui_view_find(axmui_window_root(g_win),"status");
    if(st && s) strncpy(st->text,s,sizeof(st->text)-1);
}

static int do_login(void){
    axmui_view_t *uview=axmui_view_find(axmui_window_root(g_win),"user");
    axmui_view_t *pview=axmui_view_find(axmui_window_root(g_win),"pass");
    if(!uview||!pview) return 0;
    if(uview->value[0]==0 || pview->value[0]==0){
        set_status("Enter a username and password.");
        return 0;
    }
    char user[32]; char pass[64];
    strncpy(user, uview->value, sizeof(user)-1); user[31]=0;
    strncpy(pass, pview->value, sizeof(pass)-1); pass[63]=0;
    if(sys_authenticate(user, pass)<0){
        memset(pass,0,sizeof(pass));
        login_scrub(pview);
        set_status("Login failed.");
        return 0;
    }
    memset(pass,0,sizeof(pass));
    set_status("");
    g_login_ok = 1;
    if(g_app) g_app->running=0;
    return 1;
}

/* True when /etc/passwd holds a "user" or "admin" account. */
static int has_regular_user(void){
    char text[8193];
    int fd=open("/etc/passwd",O_RDONLY);
    size_t got=0;
    long r;
    if(fd<0) return 0;
    while(got+1<sizeof(text)){
        r=read(fd,text+got,sizeof(text)-1-got);
        if(r<=0) break;
        got+=(size_t)r;
    }
    close(fd);
    text[got]=0;
    char *line=text;
    while(*line){
        char *nl=line;
        char *f[7];
        int nf=0;
        char *p=line;
        while(*nl && *nl!='\n') nl++;
        if(*nl)*nl++=0;
        while(*p && nf<7){
            f[nf++]=p;
            while(*p && *p!=':') p++;
            if(*p)*p++=0;
        }
        if(nf>=7 && f[4][0] &&
            (strcmp(f[4],"user")==0 || strcmp(f[4],"admin")==0))
            return 1;
        line=nl;
    }
    return 0;
}

static void on_login_click(axmui_view_t *v, void *ud){
    (void)v;(void)ud;
    do_login();
}

static void on_login_key(axmui_view_t *sender, void *ud){
    (void)sender;(void)ud;
    if(g_app) g_app->running=0;
}

int main(int argc, char **argv){
    (void)argc;(void)argv;

    g_app=axmui_app_create();
    if(!g_app) return 1;
    g_win=axmui_window_create(g_app,"Login", WM_WIN_W, WM_WIN_H);
    if(!g_win) return 1;

    int nologin=!has_regular_user();

    const char *html =
        "<div class='col'>"
        "  <div class='header'><h1>axiomeOS login</h1></div>"
        "  <div class='card'>"
        "    <label>Username</label>"
        "    <input id='user' placeholder='username' />"
        "    <label>Password</label>"
        "    <input id='pass' placeholder='password' />"
        "    <div class='row' style='justify-content:flex-start'>"
        "      <button id='login' class='btn-primary'>Login</button>"
        "    </div>"
        "    <p id='status' class='muted' style='color:#F87171'></p>"
        "  </div>"
        "</div>";

    const char *css =
        "label{ color:#E2E8F0; margin-top:4px; }"
        ".card{ gap:8px; }"
        "input{ width:100%; }"
        "#status{ min-height:16px; }";

    axmui_window_set_html(g_win, html, css);

    axmui_view_t *pass=axmui_view_find(axmui_window_root(g_win),"pass");
    if(pass) pass->input_secure=1;

    axmui_view_t *loginBtn=axmui_view_find(axmui_window_root(g_win),"login");
    if(loginBtn) axmui_view_on_click(loginBtn, on_login_click, NULL);

    /* Enter in an input fires on_change -> do_login (wired below). */
    axmui_view_t *user=axmui_view_find(axmui_window_root(g_win),"user");
    axmui_view_t *pv=axmui_view_find(axmui_window_root(g_win),"pass");
    if(user){ user->on_change=on_login_click; g_win->focused=user; user->focused=1; }
    if(pv) pv->on_change=on_login_click;

    if(nologin) set_status("No user account exists yet: run First-boot setup first.");

    /* Escape (or any key when not in an input) quits without logging in. */
    axmui_window_on_key(g_win, on_login_key, NULL);

    axmui_app_run(g_app, argc, argv, "axlogin");
    return g_login_ok ? 0 : 1;
}
