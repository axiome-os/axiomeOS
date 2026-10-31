/* axlogin — GUI login (axiomeOS /Binaries/axlogin)
 * Rebuilt on axmui: native HTML+CSS toolkit.
 * Shows username/password form. On success authenticates, records session
 * in /var/run/axlogin.<shmid> and exits 0 so guixd can spawn the user session.
 */
#include "axmui/axmui.h"
#include "axsha256.h"

#define BTN_LOGIN 1
#define AXLOGIN_RUN_DIR "/var/run"

static axmui_app_t *g_app;
static axmui_window_t *g_win;
static struct axclient *g_cxptr;

static void login_scrub(axmui_view_t *v){
    if(!v) return;
    memset(v->value,0,sizeof(v->value));
    memset(v->text,0,sizeof(v->text));
}

static int write_session_file(long shmid, uid_t uid, gid_t gid){
    char path[64]; char body[64]; int fd; size_t off=0, len;
    if(shmid<=0) return -1;
    mkdir(AXLOGIN_RUN_DIR);
    snprintf(path,sizeof(path),"%s/axlogin.%ld",AXLOGIN_RUN_DIR,shmid);
    snprintf(body,sizeof(body),"%u %u\n",(unsigned)uid,(unsigned)gid);
    len=strlen(body);
    fd=open(path, O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0) return -1;
    while(off<len){
        long r=write(fd, body+off, len-off);
        if(r<=0){ close(fd); return -1; }
        off+=(size_t)r;
    }
    close(fd); return 0;
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
    login_scrub(pview);
    uid_t uid; gid_t gid;
    if(sys_getpwnam(user, &uid, &gid)<0){
        set_status("Login failed.");
        return 0;
    }
    set_status("");
    if(!g_cxptr){ set_status("No window context."); return 0; }
    if(write_session_file(g_cxptr->shmid, uid, gid)<0){
        set_status("Could not record the session.");
        return 0;
    }
    return 1;
}

static void on_login_click(axmui_view_t *v, void *ud){
    (void)v; (void)ud;
    if(do_login()){
        if(g_app) g_app->running=0;
        /* exit code 0 handled by main return */
        g_app->running=0;
        /* use global flag to signal success - main loop will exit and we return 0 */
        /* we need to propagate success via a static variable */
    }
}

static int g_login_ok=0;
static void on_login_click_ok(axmui_view_t *v, void *ud){
    (void)v; (void)ud;
    if(do_login()){
        g_login_ok=1;
        if(g_app) g_app->running=0;
    }
}

int main(int argc, char **argv){
    g_app=axmui_app_create();
    if(!g_app) return 1;
    g_win=axmui_window_create(g_app,"Login", WM_WIN_W, WM_WIN_H);
    if(!g_win) return 1;

    int nouser=!axclient_has_regular_user();

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

    /* secure password field */
    axmui_view_t *pass=axmui_view_find(axmui_window_root(g_win),"pass");
    if(pass) pass->input_secure=1;

    axmui_view_t *loginBtn=axmui_view_find(axmui_window_root(g_win),"login");
    if(loginBtn) axmui_view_on_click(loginBtn, on_login_click_ok, NULL);

    /* status for no-user */
    if(nouser) set_status("No user account exists yet: run First-boot setup first.");

    /* wire Enter on inputs to login */
    axmui_view_t *user=axmui_view_find(axmui_window_root(g_win),"user");
    axmui_view_t *pv=axmui_view_find(axmui_window_root(g_win),"pass");
    if(user)  user->on_change=on_login_click_ok;
    if(pv)    pv->on_change=on_login_click_ok;

    /* focus first field */
    g_win->focused=user;
    if(user) user->focused=1;

    /* Need cx ptr for session file: after axmui_app_run attaches we can get it.
       Workaround: parse args ourselves to get shmid before run, store for write. */
    /* Pre-parse shmid */
    long shmid=-1;
    if(argc>=6 && strcmp(argv[1],"--wm")==0){
        axclient_parse_num(argv[2], &shmid);
    }
    /* create a fake client for session path before running - attach will overwrite */
    /* Instead we will capture it inside app run via g_win->cx after attach. To do that we need to intercept.
       Easiest: run custom loop that captures shmid via axclient_init ourselves, then enter axmui render loop.
       But we use axmui_app_run which does its own init and we lose shmid reference.
       So we store shmid globally and write_session_file will use that if g_cxptr null.
    */
    static long g_shmid;
    g_shmid=shmid;

    /* Custom handler that needs correct shmid: override write_session_file to use g_shmid if needed */
    /* Patch do_login to fallback */
    /* We'll do it by setting global before run and inside do_login check */
    /* To make g_cxptr available, we will run a tiny wrapper: instead of axmui_app_run, we do manual attach+loop so we can capture cx */

    /* Manual attach+loop to keep g_cxptr valid for session file */
    struct axclient cx;
    int rest=axclient_init(&cx, argc, argv, "axlogin");
    if(rest<0) return 1;
    g_cxptr=&cx;
    /* attach window fb */
    axmui_window_attach(g_win, &cx);
    /* if nouser, keep status already set; need to re-render initial frame */
    axclient_begin(&cx);
    axmui_window_render(g_win);
    cx.hdr->ready=1;
    axclient_commit(&cx);

    uint32_t prev_btn=0;
    while(g_app->running){
        struct wm_event ev;
        if(axclient_closed(&cx)||axclient_stale(&cx)) break;
        if(axgui_wm_recv(cx.evfd,&ev)<0) break;
        if(axclient_closed(&cx)||axclient_stale(&cx)) break;
        if(ev.type==WM_EV_MOUSE){
            axmui_handle_mouse(g_win, ev.x, ev.y, ev.code, &prev_btn);
        } else if(ev.type==WM_EV_KEY){
            if(ev.code=='\t'||ev.code==AXINPUT_KEY_DOWN||ev.code==AXINPUT_KEY_UP){
                int dir=(ev.code==AXINPUT_KEY_UP)?-1:1;
                axmui_view_t *list[AXMUI_MAX_NODES]; int n=0;
                for(int k=0;k<g_win->pool_used;k++){ axmui_view_t *nd=&g_win->pool[k]; if((axmui_str_case_eq(nd->tag,"input")||axmui_str_case_eq(nd->tag,"button")) && nd->style.display!=AXMUI_DISPLAY_NONE) list[n++]=nd; }
                if(n>0){ int idx=-1; for(int k=0;k<n;k++) if(list[k]==g_win->focused) idx=k; int nxt=(idx+dir+n)%n; g_win->focused=list[nxt]; for(int k=0;k<g_win->pool_used;k++) g_win->pool[k].focused=0; list[nxt]->focused=1; }
            } else {
                axmui_view_t *f=g_win->focused;
                if(f && axmui_str_case_eq(f->tag,"input")){
                    if(ev.code==127||ev.code==8){ axmui_input_backspace(f); }
                    else if(ev.code=='\n'||ev.code=='\r'){
                        if(f->on_change) f->on_change(f,f->on_change_data);
                        else if(do_login()){ g_login_ok=1; break; }
                    } else if(ev.code>=32&&ev.code<127){ axmui_input_append(f,(char)ev.code); }
                } else {
                    if(ev.code=='\n'||ev.code=='\r'){
                        if(f && axmui_str_case_eq(f->tag,"button")&&f->on_click){ f->on_click(f,f->on_click_data); }
                    }
                }
            }
        }
        axclient_begin(&cx);
        axmui_window_render(g_win);
        axclient_commit(&cx);
        if(g_login_ok) break;
    }
    return g_login_ok?0:0;
}
