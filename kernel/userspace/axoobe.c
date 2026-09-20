/* axoobe — GUI first-boot setup (axiomeOS /Binaries/axoobe)
 * Rebuilt on axmui: native HTML+CSS.
 */
#include "axmui/axmui.h"
#include "axsha256.h"

#define PASSWD_PATH "/etc/passwd"
#define USERS_DIR   "/Users"

static axmui_app_t *g_app;
static axmui_window_t *g_win;
static int g_admin=0;
static int g_created=0;

static int valid_username(const char *s){
    if(s[0]==0||strlen(s)>31) return 0;
    while(*s){ char c=*s++; if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0;}
    return 1;
}
static void set_status(const char *s){
    axmui_view_t *st=axmui_view_find(axmui_window_root(g_win),"status");
    if(st && s) strncpy(st->text,s,sizeof(st->text)-1);
}
static int write_all(int fd,const char *buf,size_t len){
    size_t off=0;
    while(off<len){ long r=write(fd,buf+off,len-off); if(r<=0) return -1; off+=(size_t)r; }
    return 0;
}
static void scrub_view(axmui_view_t *v){ if(v){ memset(v->value,0,sizeof(v->value)); memset(v->text,0,sizeof(v->text)); } }

static int create_account(void){
    axmui_view_t *uv=axmui_view_find(axmui_window_root(g_win),"user");
    axmui_view_t *pv=axmui_view_find(axmui_window_root(g_win),"pass");
    axmui_view_t *cv=axmui_view_find(axmui_window_root(g_win),"confirm");
    if(!uv||!pv||!cv) return 0;
    char *name=uv->value;
    char pass[64]; char confirm[64];
    size_t i;
    if(!valid_username(name)){ set_status("Bad username: letters, digits, _ - . (max 31)."); return 0; }
    for(i=0;i<sizeof(pass)-1 && pv->value[i];i++) pass[i]=pv->value[i]; pass[i]=0;
    for(i=0;i<sizeof(confirm)-1 && cv->value[i];i++) confirm[i]=cv->value[i]; confirm[i]=0;
    if(strlen(pass)<4){ memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm)); set_status("Password must be at least 4 characters."); return 0; }
    if(strcmp(pass,confirm)!=0){ memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm)); scrub_view(cv); set_status("Passwords do not match."); return 0; }

    char dir[160]; char old[AXCLIENT_PASSWD_MAX+1]; char entry[256]; char hash[65]; size_t old_len=0;
    snprintf(dir,sizeof(dir),"%s/%s",USERS_DIR,name);
    if(mkdir(dir)!=0){
        struct stat st;
        if(stat(dir,&st)!=0 || !(st.st_mode & S_IFDIR)){ memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm)); set_status("Could not create the home directory."); return 0; }
    }
    chown(dir,1000,1000);
    if(axclient_read_whole(PASSWD_PATH, old,sizeof(old),&old_len)<0){ memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm)); set_status("Could not read /etc/passwd."); return 0; }
    if(axclient_has_regular_user()){ memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm)); return 1; }
    axsha256_hex(pass,hash);
    memset(pass,0,sizeof(pass)); memset(confirm,0,sizeof(confirm));
    scrub_view(pv); scrub_view(cv);
    int n=snprintf(entry,sizeof(entry),"%s:%s:1000:1000:%s:%s/%s:/bin/sh\n", name, hash, g_admin?"admin":"user", USERS_DIR, name);
    if(n<=0||(size_t)n>=sizeof(entry)){ set_status("Account entry too long."); return 0; }
    int fd=open(PASSWD_PATH,O_WRONLY|O_APPEND);
    if(fd<0){ set_status("Could not update /etc/passwd."); return 0; }
    if(old_len>0 && old[old_len-1]!='\n'){ if(write_all(fd,"\n",1)<0){ close(fd); set_status("Could not update /etc/passwd."); return 0; } }
    if(write_all(fd,entry,strlen(entry))<0){ close(fd); set_status("Could not update /etc/passwd."); return 0; }
    close(fd);
    if(sys_reload_users()!=0){ set_status("Account created; user DB refresh failed."); return 0; }
    return 1;
}

static void on_create(axmui_view_t *v, void *ud){
    (void)v;(void)ud;
    if(create_account()){ set_status("Account created. Welcome to axiomeOS!"); g_created=1; if(g_app) g_app->running=0; }
}
static void on_admin_toggle(axmui_view_t *v, void *ud){
    (void)v; (void)ud;
    axmui_view_t *cb=axmui_view_find(axmui_window_root(g_win),"admin");
    if(cb) g_admin=cb->checked;
}
static void on_any_key_nouser(axmui_view_t *v, void *ud){ (void)v;(void)ud; if(g_app) g_app->running=0; }

int main(int argc,char **argv){
    if(axclient_has_regular_user()){
        /* axmui still used for the notice */
        g_app=axmui_app_create();
        g_win=axmui_window_create(g_app,"First-boot setup",WM_WIN_W, WM_WIN_H);
        const char *html="<div class='col center' style='height:100%; gap:16px'>"
                         "<div class='card' style='text-align:center; align-items:center'>"
                         "<h1>First-boot setup</h1>"
                         "<p>A user account already exists: nothing to set up.</p>"
                         "<p class='muted'>Press any key to close.</p>"
                         "<button id='close' class='btn-secondary'>Close</button>"
                         "</div></div>";
        axmui_window_set_html(g_win, html, "");
        axmui_view_t *closeBtn=axmui_view_find(axmui_window_root(g_win),"close");
        if(closeBtn) axmui_view_on_click(closeBtn, on_any_key_nouser, NULL);
        axmui_window_on_key(g_win, on_any_key_nouser, NULL);
        struct axclient cx; int rest=axclient_init(&cx,argc,argv,"axoobe"); if(rest<0) return 0;
        axmui_window_attach(g_win,&cx);
        axclient_begin(&cx); axmui_window_render(g_win); cx.hdr->ready=1; axclient_commit(&cx);
        uint32_t prev_btn2=0;
        while(g_app->running){
            struct wm_event ev; if(axclient_closed(&cx)||axclient_stale(&cx)) break; if(axgui_wm_recv(cx.evfd,&ev)<0) break;
            if(ev.type==WM_EV_MOUSE){
                axmui_handle_mouse(g_win, ev.x, ev.y, ev.code, &prev_btn2);
            } else if(ev.type==WM_EV_KEY){ break; }
            axclient_begin(&cx); axmui_window_render(g_win); axclient_commit(&cx);
        }
        return 0;
    }

    g_app=axmui_app_create();
    g_win=axmui_window_create(g_app,"First-boot setup",WM_WIN_W,WM_WIN_H);
    const char *html=
        "<div class='col'>"
        "  <div class='header'><h1>axiomeOS first-boot setup</h1></div>"
        "  <div class='card'>"
        "    <label>Username</label><input id='user' placeholder='letters, digits, _ - . (max 31)' />"
        "    <label>Password</label><input id='pass' placeholder='at least 4 characters' />"
        "    <label>Confirm password</label><input id='confirm' placeholder='repeat password' />"
        "    <div class='row' style='align-items:center; margin-top:6px'>"
        "      <input type='checkbox' id='admin' />"
        "      <label>Administrator</label>"
        "    </div>"
        "    <div class='row' style='margin-top:4px'><button id='create' class='btn-primary'>Create account</button></div>"
        "    <p id='status' class='muted' style='color:#F87171; min-height:16px'></p>"
        "  </div>"
        "</div>";
    const char *css="label{ color:#E2E8F0; } .card{ gap:8px; } #user{ width:100%; } #pass{ width:100%; } #confirm{ width:100%; }";
    axmui_window_set_html(g_win, html, css);
    axmui_view_t *pv=axmui_view_find(axmui_window_root(g_win),"pass");
    axmui_view_t *cv=axmui_view_find(axmui_window_root(g_win),"confirm");
    if(pv) pv->input_secure=1;
    if(cv) cv->input_secure=1;
    axmui_view_t *adminBtn=axmui_view_find(axmui_window_root(g_win),"admin");
    axmui_view_t *createBtn=axmui_view_find(axmui_window_root(g_win),"create");
    if(adminBtn) axmui_view_on_click(adminBtn, on_admin_toggle, NULL);
    if(createBtn) axmui_view_on_click(createBtn, on_create, NULL);

    /* focus first input */
    axmui_view_t *user=axmui_view_find(axmui_window_root(g_win),"user");
    if(user){ g_win->focused=user; user->focused=1; }

    /* custom loop that also handles Enter on inputs via on_change */
    struct axclient cx; int rest=axclient_init(&cx,argc,argv,"axoobe"); if(rest<0) return 1;
    axmui_window_attach(g_win,&cx);
    axclient_begin(&cx); axmui_window_render(g_win); cx.hdr->ready=1; axclient_commit(&cx);
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
                for(int k=0;k<g_win->pool_used;k++){ axmui_view_t *nd=&g_win->pool[k]; if((axmui_str_case_eq(nd->tag,"input")||axmui_str_case_eq(nd->tag,"button"))&&nd->style.display!=AXMUI_DISPLAY_NONE) list[n++]=nd; }
                if(n>0){ int idx=-1; for(int k=0;k<n;k++) if(list[k]==g_win->focused) idx=k; int nxt=(idx+dir+n)%n; g_win->focused=list[nxt]; for(int k=0;k<g_win->pool_used;k++) g_win->pool[k].focused=0; list[nxt]->focused=1; }
            } else {
                axmui_view_t *f=g_win->focused;
                if(f && axmui_str_case_eq(f->tag,"input")){
                    if(ev.code==127||ev.code==8) axmui_input_backspace(f);
                    else if(ev.code=='\n'||ev.code=='\r'){ if(create_account()){ set_status("Account created."); axclient_begin(&cx); axmui_window_render(g_win); axclient_commit(&cx); return 0; } }
                    else if(ev.code>=32&&ev.code<127) axmui_input_append(f,(char)ev.code);
                } else {
                    if(ev.code=='\n'||ev.code=='\r'){ if(f && axmui_str_case_eq(f->tag,"button")&&f->on_click) f->on_click(f,f->on_click_data); }
                }
            }
        }
        axclient_begin(&cx); axmui_window_render(g_win); axclient_commit(&cx);
        if(g_created) return 0;
    }
    return g_created?0:1;
}
