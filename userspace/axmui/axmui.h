/* axmui.h — axiomeOS native HTML+CSS UI toolkit.
 *
 * A beautiful, declarative, native UI library (no WebKit, no browser engine).
 * HTML describes structure, CSS describes appearance, C describes behaviour —
 * same mental model as the web, but every pixel is rasterized natively into
 * the window SHM framebuffer via axgui primitives and presented through the
 * guixd compositor (axclient/window SHM + pipe).
 *
 * API is Cocoa/GTK-like (views, windows, app) with an imperative C flavour:
 *   axmui_app_t *app = axmui_app_create();
 *   axmui_window_t *win = axmui_window_create(app,"Title",620,420);
 *   axmui_view_t *root = axmui_window_root(win);
 *   // declarative (HTML+CSS):
 *   axmui_window_set_html(win, "<div class='card'><h1>Hi</h1></div>",
 *                              ".card{ background:#1e2a3a; border-radius:12px; padding:16px; }");
 *   // or imperative:
 *   axmui_view_t *btn = axmui_button_create("OK");
 *   axmui_view_on_click(btn, on_ok, NULL);
 *   axmui_view_append(root, btn);
 *   axmui_app_run(app);
 *
 * Header-only like axgui.h/axclient.h/axform.h so existing USER_PROGS rules
 * link it automatically (no extra Makefile tail). Include once per program.
 *
 * Rendering model: pure software raster on the SHM dumb buffer
 * (0x00RRGGBB). Rounded rectangles, borders, text and backgrounds are drawn
 * natively — there is no embedded HTML engine.
 *
 * CSS subset implemented (enough for beautiful app UIs):
 *   selectors: tag, .class, #id, *, tag.class, descendant "A B", comma groups,
 *              pseudo :hover :active :focus
 *   properties: display(block/flex/none), flex-direction, justify-content,
 *               align-items, flex/gap, width/height (px/%/auto), min/max,
 *               margin, padding, background/background-color, color,
 *               border, border-radius, font-size, font-weight, text-align,
 *               opacity, visibility
 *   colors: #RGB #RRGGBB rgb(), named white/black/transparent + theme names
 *   units: px, %, auto
 *
 * Layout: block flow (vertical stack) + flexbox (row/column) with gap and
 *         flex-grow. Box model: content + padding + border + margin.
 *         Integer px arithmetic (no float), window is 620x420 fixed.
 */

#ifndef AXMUI_H
#define AXMUI_H

#include "../axgui.h"
#include "../axclient.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
#define AXMUI_MAX_NODES      128
#define AXMUI_MAX_RULES      64
#define AXMUI_MAX_DECLS      8
#define AXMUI_MAX_CHILDREN   32
#define AXMUI_TEXT_CAP       256
#define AXMUI_CLASS_CAP      96
#define AXMUI_ID_CAP         32
#define AXMUI_STYLE_CAP      256
#define AXMUI_TAG_CAP        16

/* ------------------------------------------------------------------ */
/*  Forward types                                                      */
struct axmui_node;
struct axmui_window;
struct axmui_app;
typedef struct axmui_node axmui_view_t;
typedef struct axmui_window axmui_window_t;
typedef struct axmui_app axmui_app_t;

typedef void (*axmui_callback_t)(axmui_view_t *sender, void *userdata);
typedef void (*axmui_draw_fn_t)(struct axgui_fb *fb, axmui_view_t *view, void *ud);

/* ------------------------------------------------------------------ */
/*  Style                                                              */
enum {
    AXMUI_DISPLAY_BLOCK = 0,
    AXMUI_DISPLAY_FLEX  = 1,
    AXMUI_DISPLAY_NONE  = 2,
};
enum {
    AXMUI_FLEX_ROW = 0,
    AXMUI_FLEX_COL = 1,
};
enum {
    AXMUI_JUSTIFY_START = 0,
    AXMUI_JUSTIFY_CENTER= 1,
    AXMUI_JUSTIFY_END   = 2,
    AXMUI_JUSTIFY_BETWEEN=3,
    AXMUI_JUSTIFY_AROUND =4,
};
enum {
    AXMUI_ALIGN_STRETCH=0,
    AXMUI_ALIGN_START=1,
    AXMUI_ALIGN_CENTER=2,
    AXMUI_ALIGN_END=3,
};
enum {
    AXMUI_TEXT_LEFT=0,
    AXMUI_TEXT_CENTER=1,
    AXMUI_TEXT_RIGHT=2,
};

struct axmui_style {
    int display;
    int flex_dir;
    int justify;
    int align;
    int flex_grow;          /* 0..100, 0 = not flex, >0 weighted */
    int gap;                /* px */

    int width; int width_pct; int width_auto;
    int height; int height_pct; int height_auto;
    int min_w; int max_w;
    int margin[4];  /* top,right,bottom,left */
    int padding[4];
    int border_w;
    uint32_t border_color;
    int radius;
    uint32_t bg;    /* 0x00RRGGBB ; 0xFFFFFFFF = transparent flag */
    uint32_t fg;
    int font_size;  /* 8 or 16: maps to FONT_WIDTH/H */
    int font_bold;
    int text_align;
    int opacity;    /* 0..100 */
    int visible;    /* 1 visible 0 invisible */
};

/* ------------------------------------------------------------------ */
/*  DOM node                                                           */
struct axmui_node {
    char tag[AXMUI_TAG_CAP];
    char id[AXMUI_ID_CAP];
    char klass[AXMUI_CLASS_CAP];
    char text[AXMUI_TEXT_CAP];          /* element text or text-node content */
    char placeholder[64];
    char value[AXMUI_TEXT_CAP];
    int is_text;
    int input_secure;                   /* 1 for password */
    int is_checkbox;                    /* input type=checkbox */
    int checked;
    int disabled;

    struct axmui_node *parent;
    struct axmui_node *first_child;
    struct axmui_node *last_child;
    struct axmui_node *next_sibling;
    struct axmui_node *prev_sibling;

    struct axmui_style style;           /* computed */
    char inline_style[AXMUI_STYLE_CAP]; /* raw style="..." */

    /* layout */
    int x, y, w, h;                     /* content box pos/size relative to parent content */
    int abs_x, abs_y;
    int margin[4];
    int padding[4];

    /* state for pseudo */
    int hovered;
    int active;
    int focused;
    int pseudo_hover;
    int pseudo_active;
    int pseudo_focus;

    /* events */
    axmui_callback_t on_click;
    void *on_click_data;
    axmui_callback_t on_input;
    void *on_input_data;
    axmui_callback_t on_change;
    void *on_change_data;

    /* custom draw */
    axmui_draw_fn_t custom_draw;
    void *custom_data;
};

/* ------------------------------------------------------------------ */
/*  CSS sheet                                                          */
struct axmui_decl {
    char prop[32];
    char val[64];
};
struct axmui_rule {
    char selector[96];
    struct axmui_decl decls[AXMUI_MAX_DECLS];
    int ndecls;
    int pseudo; /* 0 none 1 hover 2 active 3 focus */
};
struct axmui_sheet {
    struct axmui_rule rules[AXMUI_MAX_RULES];
    int nrules;
};

/* ------------------------------------------------------------------ */
/*  Window / App                                                        */
struct axmui_window {
    struct axclient cx;
    axmui_view_t *root;
    struct axmui_sheet sheet;
    axmui_view_t *focused;  /* input */
    axmui_view_t *hovered;
    axmui_view_t *active;
    int needs_layout;
    int needs_render;
    char title[48];
    /* pool: simple bump allocator for nodes (no per-node malloc churn) */
    struct axmui_node pool[AXMUI_MAX_NODES];
    int pool_used;
    /* raw storage for window */
    struct axgui_fb fb;
    /* window-level key handler (e.g. axinfo any-key-to-close) */
    axmui_callback_t on_key;
    void *on_key_data;
};

struct axmui_app {
    axmui_window_t *win;
    int running;
};

/* caps helpers (GUI asks hdr for gfx caps) */
static inline uint64_t axmui_window_caps(axmui_window_t *win)
{
    if (!win || !win->cx.hdr) return 0;
    return win->cx.hdr->gfx_caps;
}
static inline int axmui_has_cap(axmui_window_t *win, uint64_t cap)
{
    return (axmui_window_caps(win) & cap) != 0;
}
static inline const char *axmui_detail_mode(axmui_window_t *win)
{
    if (!win || !win->cx.hdr) return "simplified";
    return win->cx.hdr->gfx_detail ? "detailed" : "simplified";
}
static inline int axmui_should_blur(axmui_window_t *win)
{
    return axmui_has_cap(win, WM_GFX_CAP_BLUR);
}
static inline int axmui_should_shadow(axmui_window_t *win)
{
    return axmui_has_cap(win, WM_GFX_CAP_SHADOWS);
}

/* ================================================================== */
/*  Helpers (static)                                                   */

static int axmui_is_space(char c){ return c==' '||c=='\n'||c=='\r'||c=='\t'; }
static int axmui_is_alpha(char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
static int axmui_is_digit(char c){ return c>='0'&&c<='9'; }
static int axmui_str_eq(const char *a,const char *b){ return strcmp(a,b)==0; }
static int axmui_str_case_eq(const char *a,const char *b){
    while(*a&&*b){
        char ca=*a, cb=*b;
        if(ca>='A'&&ca<='Z') ca+=32;
        if(cb>='A'&&cb<='Z') cb+=32;
        if(ca!=cb) return 0;
        a++; b++;
    }
    return *a==*b;
}
static void axmui_trim(char *s){
    size_t i, n=strlen(s);
    size_t st=0;
    while(st<n && axmui_is_space(s[st])) st++;
    while(n>st && axmui_is_space(s[n-1])) n--;
    for(i=0;i+st<n;i++) s[i]=s[st+i];
    s[i]=0;
}

/* color parsing ---------------------------------------------------- */
static uint32_t axmui_parse_hex1(char c){
    if(c>='0'&&c<='9') return (uint32_t)(c-'0');
    if(c>='a'&&c<='f') return (uint32_t)(c-'a'+10);
    if(c>='A'&&c<='F') return (uint32_t)(c-'A'+10);
    return 0;
}
static int axmui_parse_color(const char *s, uint32_t *out){
    size_t i; if(!s||!out) return -1;
    while(axmui_is_space(*s)) s++;
    if(s[0]=='#'){
        size_t len=strlen(s);
        if(len==4){ /* #RGB */
            uint32_t r=axmui_parse_hex1(s[1]); r=(r<<4)|r;
            uint32_t g=axmui_parse_hex1(s[2]); g=(g<<4)|g;
            uint32_t b=axmui_parse_hex1(s[3]); b=(b<<4)|b;
            *out=(r<<16)|(g<<8)|b; return 0;
        } else if(len>=7){ /* #RRGGBB */
            uint32_t r=(axmui_parse_hex1(s[1])<<4)|axmui_parse_hex1(s[2]);
            uint32_t g=(axmui_parse_hex1(s[3])<<4)|axmui_parse_hex1(s[4]);
            uint32_t b=(axmui_parse_hex1(s[5])<<4)|axmui_parse_hex1(s[6]);
            *out=(r<<16)|(g<<8)|b; return 0;
        }
        return -1;
    }
    if(strncmp(s,"rgb",3)==0){
        const char *p=strchr(s,'('); if(!p) return -1;
        long r=0,g=0,b=0; char *e;
        r=atol(p+1); p=strchr(p+1,','); if(!p) return -1;
        g=atol(p+1); p=strchr(p+1,','); if(!p) return -1;
        b=atol(p+1);
        if(r<0) r=0; if(r>255) r=255; if(g<0) g=0; if(g>255) g=255; if(b<0) b=0; if(b>255) b=255;
        (void)e;
        *out=((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b; return 0;
    }
    if(axmui_str_case_eq(s,"white")){ *out=0xFFFFFFu; return 0; }
    if(axmui_str_case_eq(s,"black")){ *out=0x000000u; return 0; }
    if(axmui_str_case_eq(s,"transparent")){ *out=0xFFFFFFFFu; return 0; }
    if(axmui_str_case_eq(s,"red")){ *out=0xE74C3Cu; return 0; }
    if(axmui_str_case_eq(s,"blue")){ *out=0x3498DBu; return 0; }
    if(axmui_str_case_eq(s,"green")){ *out=0x2ECC71u; return 0; }
    /* theme aliases */
    if(axmui_str_case_eq(s,"surface")){ *out=0x1E2A3Au; return 0; }
    if(axmui_str_case_eq(s,"card")){ *out=0x243447u; return 0; }
    for(i=0;s[i];i++) if(s[i]==','||s[i]=='(') break;
    return -1;
}

static int axmui_parse_len(const char *s, int *px, int *is_pct, int *is_auto){
    if(!s||!px) return -1;
    while(axmui_is_space(*s)) s++;
    if(axmui_str_case_eq(s,"auto")){
        if(is_auto) *is_auto=1;
        if(is_pct) *is_pct=0;
        *px=0; return 0;
    }
    size_t len=strlen(s);
    if(len>0 && s[len-1]=='%'){
        char tmp[32]; size_t i;
        for(i=0;i+1<len && i+1<sizeof(tmp);i++) tmp[i]=s[i];
        tmp[i]=0; *px=atoi(tmp); if(is_pct) *is_pct=1; if(is_auto) *is_auto=0; return 0;
    }
    if(len>=2 && s[len-2]=='p' && s[len-1]=='x'){
        char tmp[32]; size_t i;
        for(i=0;i+2<len+2&&i+2<sizeof(tmp);i++) tmp[i]=s[i]; /* copy without px later */
        /* simpler: atoi handles px suffix as stop, but we strip */
        char copy[32]; strncpy(copy,s,31); copy[31]=0;
        char *pxpos=strstr(copy,"px"); if(pxpos) *pxpos=0;
        *px=atoi(copy); if(is_pct) *is_pct=0; if(is_auto) *is_auto=0; return 0;
    }
    /* bare number = px */
    *px=atoi(s); if(is_pct) *is_pct=0; if(is_auto) *is_auto=0; return 0;
}

/* bbox helpers */
static void axmui_style_init(struct axmui_style *s){
    memset(s,0,sizeof(*s));
    s->display=AXMUI_DISPLAY_BLOCK;
    s->flex_dir=AXMUI_FLEX_COL;
    s->justify=AXMUI_JUSTIFY_START;
    s->align=AXMUI_ALIGN_STRETCH;
    s->flex_grow=0;
    s->gap=0;
    s->width_auto=1; s->height_auto=1;
    s->width=0; s->height=0;
    s->min_w=-1; s->max_w=-1;
    s->margin[0]=s->margin[1]=s->margin[2]=s->margin[3]=0;
    s->padding[0]=s->padding[1]=s->padding[2]=s->padding[3]=0;
    s->border_w=0; s->border_color=0x2B3B55u;
    s->radius=0;
    s->bg=0xFFFFFFFFu; /* transparent */
    s->fg=0xD8DEE9u;
    s->font_size=FONT_HEIGHT;
    s->font_bold=0;
    s->text_align=AXMUI_TEXT_LEFT;
    s->opacity=100;
    s->visible=1;
}

static int axmui_apply_decl(struct axmui_style *st, const char *prop, const char *val){
    if(!st||!prop||!val) return -1;
    char p[32]; char v[64];
    size_t i;
    for(i=0;i<31&&prop[i];i++) p[i]= (prop[i]>='A'&&prop[i]<='Z')? prop[i]+32:prop[i];
    p[i]=0; strncpy(v,val,63); v[63]=0; axmui_trim(v);
    if(strcmp(p,"display")==0){
        if(axmui_str_case_eq(v,"flex")) st->display=AXMUI_DISPLAY_FLEX;
        else if(axmui_str_case_eq(v,"none")) st->display=AXMUI_DISPLAY_NONE;
        else st->display=AXMUI_DISPLAY_BLOCK;
        return 0;
    }
    if(strcmp(p,"flex-direction")==0){
        if(axmui_str_case_eq(v,"row")) st->flex_dir=AXMUI_FLEX_ROW;
        else st->flex_dir=AXMUI_FLEX_COL;
        return 0;
    }
    if(strcmp(p,"justify-content")==0){
        if(axmui_str_case_eq(v,"center")) st->justify=AXMUI_JUSTIFY_CENTER;
        else if(axmui_str_case_eq(v,"flex-end")||axmui_str_case_eq(v,"end")) st->justify=AXMUI_JUSTIFY_END;
        else if(axmui_str_case_eq(v,"space-between")) st->justify=AXMUI_JUSTIFY_BETWEEN;
        else if(axmui_str_case_eq(v,"space-around")) st->justify=AXMUI_JUSTIFY_AROUND;
        else st->justify=AXMUI_JUSTIFY_START;
        return 0;
    }
    if(strcmp(p,"align-items")==0){
        if(axmui_str_case_eq(v,"center")) st->align=AXMUI_ALIGN_CENTER;
        else if(axmui_str_case_eq(v,"flex-start")||axmui_str_case_eq(v,"start")) st->align=AXMUI_ALIGN_START;
        else if(axmui_str_case_eq(v,"flex-end")||axmui_str_case_eq(v,"end")) st->align=AXMUI_ALIGN_END;
        else st->align=AXMUI_ALIGN_STRETCH;
        return 0;
    }
    if(strcmp(p,"flex")==0 || strcmp(p,"flex-grow")==0){
        st->flex_grow=atoi(v); if(st->flex_grow<0) st->flex_grow=0;
        return 0;
    }
    if(strcmp(p,"gap")==0){
        int px, pct, aut; axmui_parse_len(v,&px,&pct,&aut); st->gap=px; return 0;
    }
    if(strcmp(p,"width")==0){
        int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut);
        st->width=px; st->width_pct=pct; st->width_auto=aut; return 0;
    }
    if(strcmp(p,"height")==0){
        int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut);
        st->height=px; st->height_pct=pct; st->height_auto=aut; return 0;
    }
    if(strcmp(p,"margin")==0){
        int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut);
        st->margin[0]=st->margin[1]=st->margin[2]=st->margin[3]=px; return 0;
    }
    if(strcmp(p,"margin-top")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->margin[0]=px; return 0; }
    if(strcmp(p,"margin-right")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->margin[1]=px; return 0; }
    if(strcmp(p,"margin-bottom")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->margin[2]=px; return 0; }
    if(strcmp(p,"margin-left")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->margin[3]=px; return 0; }
    if(strcmp(p,"padding")==0){
        int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut);
        st->padding[0]=st->padding[1]=st->padding[2]=st->padding[3]=px; return 0;
    }
    if(strcmp(p,"padding-top")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->padding[0]=px; return 0; }
    if(strcmp(p,"padding-right")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->padding[1]=px; return 0; }
    if(strcmp(p,"padding-bottom")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->padding[2]=px; return 0; }
    if(strcmp(p,"padding-left")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->padding[3]=px; return 0; }
    if(strcmp(p,"background")==0 || strcmp(p,"background-color")==0){
        uint32_t c; if(axmui_parse_color(v,&c)==0) st->bg=c;
        return 0;
    }
    if(strcmp(p,"color")==0){
        uint32_t c; if(axmui_parse_color(v,&c)==0) st->fg=c;
        return 0;
    }
    if(strcmp(p,"border")==0){
        /* expect "1px solid #rrggbb" — parse first int and last color */
        char *sp=strchr(v,' '); uint32_t c;
        int w=atoi(v); st->border_w=w;
        if(sp){
            char *col=strrchr(v,' '); if(col && axmui_parse_color(col+1,&c)==0) st->border_color=c;
        }
        return 0;
    }
    if(strcmp(p,"border-width")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->border_w=px; return 0; }
    if(strcmp(p,"border-color")==0){ uint32_t c; if(axmui_parse_color(v,&c)==0) st->border_color=c; return 0; }
    if(strcmp(p,"border-radius")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->radius=px; if(st->radius<0) st->radius=0; return 0; }
    if(strcmp(p,"font-size")==0){ int px,pct,aut; axmui_parse_len(v,&px,&pct,&aut); st->font_size=px>8?FONT_HEIGHT:8; return 0; }
    if(strcmp(p,"font-weight")==0){ st->font_bold = (axmui_str_case_eq(v,"bold")||atoi(v)>=600); return 0; }
    if(strcmp(p,"text-align")==0){
        if(axmui_str_case_eq(v,"center")) st->text_align=AXMUI_TEXT_CENTER;
        else if(axmui_str_case_eq(v,"right")) st->text_align=AXMUI_TEXT_RIGHT;
        else st->text_align=AXMUI_TEXT_LEFT;
        return 0;
    }
    if(strcmp(p,"opacity")==0){ st->opacity=atoi(v); return 0; }
    if(strcmp(p,"visibility")==0){ st->visible=!axmui_str_case_eq(v,"hidden"); return 0; }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  CSS sheet parsing                                                  */

static void axmui_sheet_init(struct axmui_sheet *sh){ memset(sh,0,sizeof(*sh)); }

static int axmui_css_parse(struct axmui_sheet *sh, const char *css){
    const char *p=css;
    if(!sh||!css) return -1;
    while(*p){
        while(axmui_is_space(*p)) p++;
        if(!*p) break;
        if(p[0]=='/' && p[1]=='*'){ /* comment */
            p+=2; while(*p && !(p[0]=='*'&&p[1]=='/')) p++;
            if(*p) p+=2; continue;
        }
        const char *sel_start=p;
        const char *brace=strchr(p,'{');
        if(!brace) break;
        size_t sel_len=(size_t)(brace - sel_start);
        const char *block_end=strchr(brace,'}');
        if(!block_end) break;
        size_t block_len=(size_t)(block_end - brace -1);
        char selbuf[96]; char blockbuf[512];
        if(sel_len>=sizeof(selbuf)) sel_len=sizeof(selbuf)-1;
        memcpy(selbuf, sel_start, sel_len); selbuf[sel_len]=0; axmui_trim(selbuf);
        if(block_len>=sizeof(blockbuf)) block_len=sizeof(blockbuf)-1;
        memcpy(blockbuf, brace+1, block_len); blockbuf[block_len]=0;
        /* split selectors by comma */
        char *save=selbuf;
        /* we reuse manual split */
        char *cur=selbuf;
        while(cur && *cur){
            while(axmui_is_space(*cur)) cur++;
            char *comma=strchr(cur,',');
            if(comma) *comma=0;
            axmui_trim(cur);
            if(*cur && sh->nrules < AXMUI_MAX_RULES){
                struct axmui_rule *r=&sh->rules[sh->nrules++];
                memset(r,0,sizeof(*r));
                /* detect pseudo */
                char *colon=strchr(cur,':');
                if(colon){
                    if(strncmp(colon+1,"hover",5)==0) r->pseudo=1;
                    else if(strncmp(colon+1,"active",6)==0) r->pseudo=2;
                    else if(strncmp(colon+1,"focus",5)==0) r->pseudo=3;
                    *colon=0;
                }
                strncpy(r->selector, cur, sizeof(r->selector)-1);
                axmui_trim(r->selector);
                /* parse decls */
                char *b=blockbuf;
                while(*b){
                    while(axmui_is_space(*b)) b++;
                    if(!*b) break;
                    char *semi=strchr(b,';');
                    if(semi) *semi=0;
                    char *colon2=strchr(b,':');
                    if(colon2){
                        *colon2=0;
                        char prop[32]; char val[64];
                        strncpy(prop,b,31); prop[31]=0; axmui_trim(prop);
                        strncpy(val,colon2+1,63); val[63]=0; axmui_trim(val);
                        if(r->ndecls < AXMUI_MAX_DECLS){
                            strncpy(r->decls[r->ndecls].prop, prop,31);
                            strncpy(r->decls[r->ndecls].val, val,63);
                            r->ndecls++;
                        }
                    }
                    if(!semi) break;
                    b=semi+1;
                }
            }
            if(!comma) break;
            cur=comma+1;
        }
        (void)save;
        p=block_end+1;
    }
    return 0;
}

/* selector matching */
static int axmui_has_class(const char *klass, const char *name){
    size_t nlen=strlen(name);
    const char *p=klass;
    while(*p){
        while(axmui_is_space(*p)) p++;
        if(!*p) break;
        const char *s=p;
        while(*p && !axmui_is_space(*p)) p++;
        size_t l=(size_t)(p - s);
        if(l==nlen && strncmp(s,name,nlen)==0) return 1;
    }
    return 0;
}
static int axmui_match_simple(axmui_view_t *node, const char *sel){
    if(!node||!sel||!*sel) return 0;
    if(strcmp(sel,"*")==0) return 1;
    /* id */
    if(sel[0]=='#'){
        return axmui_str_case_eq(node->id, sel+1);
    }
    /* class */
    if(sel[0]=='.'){
        return axmui_has_class(node->klass, sel+1);
    }
    /* tag.class or tag#id */
    char *dot=strchr(sel,'.');
    char *hash=strchr(sel,'#');
    if(dot){
        char tag[16]; size_t tl=(size_t)(dot - sel);
        if(tl>=sizeof(tag)) tl=sizeof(tag)-1;
        memcpy(tag,sel,tl); tag[tl]=0;
        if(!axmui_str_case_eq(node->tag,tag)) return 0;
        return axmui_has_class(node->klass, dot+1);
    }
    if(hash){
        char tag[16]; size_t tl=(size_t)(hash - sel);
        if(tl>=sizeof(tag)) tl=sizeof(tag)-1;
        memcpy(tag,sel,tl); tag[tl]=0;
        if(!axmui_str_case_eq(node->tag,tag)) return 0;
        return axmui_str_case_eq(node->id, hash+1);
    }
    return axmui_str_case_eq(node->tag, sel);
}
static int axmui_match_selector(axmui_view_t *node, const char *selector){
    if(!selector||!*selector) return 0;
    /* descendant: split by space, last must match node, earlier must match ancestor */
    char buf[96]; strncpy(buf,selector,95); buf[95]=0; axmui_trim(buf);
    /* count tokens */
    char *tokens[8]; int nt=0;
    char *cur=buf;
    while(*cur && nt<8){
        while(axmui_is_space(*cur)) cur++;
        if(!*cur) break;
        tokens[nt++]=cur;
        while(*cur && !axmui_is_space(*cur)) cur++;
        if(*cur){ *cur=0; cur++; }
    }
    if(nt==0) return 0;
    if(nt==1) return axmui_match_simple(node, tokens[0]);
    /* descendant: walk ancestors */
    if(!axmui_match_simple(node, tokens[nt-1])) return 0;
    axmui_view_t *anc=node->parent;
    int ti=nt-2;
    while(anc && ti>=0){
        if(axmui_match_simple(anc, tokens[ti])) ti--;
        anc=anc->parent;
    }
    return ti<0;
}

/* apply sheet to node tree */
static void axmui_compute_style_for(axmui_view_t *node, struct axmui_sheet *sh){
    int i,j;
    if(!node) return;
    axmui_style_init(&node->style);
    /* UA defaults per tag */
    if(axmui_str_case_eq(node->tag,"h1")||axmui_str_case_eq(node->tag,"h2")||axmui_str_case_eq(node->tag,"h3")){
        node->style.font_bold=1;
        node->style.padding[2]=4;
    }
    if(axmui_str_case_eq(node->tag,"button")){
        node->style.display=AXMUI_DISPLAY_FLEX;
        node->style.flex_dir=AXMUI_FLEX_ROW;
        node->style.justify=AXMUI_JUSTIFY_CENTER;
        node->style.align=AXMUI_ALIGN_CENTER;
        node->style.bg=0x2B3B55u;
        node->style.fg=0xD8DEE9u;
        node->style.padding[0]=6; node->style.padding[1]=14; node->style.padding[2]=6; node->style.padding[3]=14;
        node->style.radius=8;
        node->style.border_w=1; node->style.border_color=0x3D5A80u;
        node->style.text_align=AXMUI_TEXT_CENTER;
    }
    if(axmui_str_case_eq(node->tag,"input")){
        if(node->is_checkbox){
            node->style.bg=0x0B0F14u;
            node->style.fg=0xD8DEE9u;
            node->style.padding[0]=0; node->style.padding[1]=0; node->style.padding[2]=0; node->style.padding[3]=0;
            node->style.border_w=1; node->style.border_color=0x475569u;
            node->style.radius=4;
            node->style.width=18; node->style.width_auto=0; node->style.width_pct=0;
            node->style.height=18; node->style.height_auto=0;
            if(node->hovered) node->style.border_color=0x64748Bu;
            if(node->checked){
                node->style.bg=0x3B82F6u;
                node->style.border_color=0x2563EBu;
            }
        } else {
            node->style.bg=0x0B0F14u;
            node->style.fg=0xD8DEE9u;
            node->style.padding[0]=6; node->style.padding[1]=10; node->style.padding[2]=6; node->style.padding[3]=10;
            node->style.border_w=1; node->style.border_color=0x3D5A80u;
            node->style.radius=8;
            node->style.width=240; node->style.width_auto=0; node->style.width_pct=0;
            node->style.height=28; node->style.height_auto=0;
        }
    }
    if(axmui_str_case_eq(node->tag,"card")||axmui_has_class(node->klass,"card")){
        /* cards have elevated surface */
    }
    /* sheet rules */
    for(i=0;i<sh->nrules;i++){
        struct axmui_rule *r=&sh->rules[i];
        if(r->pseudo==1 && !node->hovered) continue;
        if(r->pseudo==2 && !node->active) continue;
        if(r->pseudo==3 && !node->focused) continue;
        if(!axmui_match_selector(node, r->selector)) continue;
        for(j=0;j<r->ndecls;j++){
            axmui_apply_decl(&node->style, r->decls[j].prop, r->decls[j].val);
        }
    }
    /* inline style highest */
    if(node->inline_style[0]){
        char buf[AXMUI_STYLE_CAP]; strncpy(buf, node->inline_style, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        char *p=buf;
        while(*p){
            while(axmui_is_space(*p)) p++;
            if(!*p) break;
            char *semi=strchr(p,';'); if(semi) *semi=0;
            char *colon=strchr(p,':');
            if(colon){ *colon=0; char prop[32]; char val[64];
                strncpy(prop,p,31); prop[31]=0; axmui_trim(prop);
                strncpy(val,colon+1,63); val[63]=0; axmui_trim(val);
                axmui_apply_decl(&node->style, prop, val);
            }
            if(!semi) break; p=semi+1;
        }
    }
    /* pseudo adjustments for interactive */
    if(node->active && axmui_str_case_eq(node->tag,"button")){
        /* darken bg on active */
        if(node->style.bg!=0xFFFFFFFFu){
            uint32_t c=node->style.bg;
            int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
            r = r*85/100; g=g*85/100; b=b*85/100;
            node->style.bg=((uint32_t)r<<16)|((uint32_t)g<<8)|b;
        }
    }
    if(node->hovered && axmui_str_case_eq(node->tag,"button")){
        if(node->style.bg!=0xFFFFFFFFu && !node->active){
            uint32_t c=node->style.bg;
            int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
            r = r + (255-r)*10/100; g=g+(255-g)*10/100; b=b+(255-b)*10/100;
            if(r>255) r=255; if(g>255) g=255; if(b>255) b=255;
            node->style.bg=((uint32_t)r<<16)|((uint32_t)g<<8)|b;
        }
    }
    if(node->focused && axmui_str_case_eq(node->tag,"input")){
        node->style.border_color=0x5E81ACu;
        node->style.border_w=2;
    }
}
static void axmui_compute_styles(axmui_view_t *root, struct axmui_sheet *sh){
    if(!root) return;
    axmui_compute_style_for(root, sh);
    for(axmui_view_t *c=root->first_child;c;c=c->next_sibling) axmui_compute_styles(c, sh);
}

/* ------------------------------------------------------------------ */
/*  HTML parsing (tiny, handles well-formed subset)                   */

static axmui_view_t *axmui_alloc_node(axmui_window_t *win){
    if(!win || win->pool_used >= AXMUI_MAX_NODES) return 0;
    axmui_view_t *n=&win->pool[win->pool_used++];
    memset(n,0,sizeof(*n));
    axmui_style_init(&n->style);
    return n;
}
static void axmui_append_child(axmui_view_t *parent, axmui_view_t *child){
    if(!parent||!child) return;
    child->parent=parent;
    child->next_sibling=0; child->prev_sibling=parent->last_child;
    if(parent->last_child) parent->last_child->next_sibling=child;
    else parent->first_child=child;
    parent->last_child=child;
}
static int axmui_is_void_tag(const char *tag){
    return axmui_str_case_eq(tag,"input")||axmui_str_case_eq(tag,"br")||
           axmui_str_case_eq(tag,"img")||axmui_str_case_eq(tag,"hr");
}
static void axmui_parse_attrs(const char *s, axmui_view_t *node){
    const char *p=s;
    while(*p){
        while(axmui_is_space(*p)) p++;
        if(!*p || *p=='/'||*p=='>') break;
        const char *name=p;
        while(*p && !axmui_is_space(*p) && *p!='=' && *p!='>' && *p!='/') p++;
        size_t nlen=(size_t)(p - name);
        while(axmui_is_space(*p)) p++;
        char nbuf[32]; if(nlen>=sizeof(nbuf)) nlen=sizeof(nbuf)-1; memcpy(nbuf,name,nlen); nbuf[nlen]=0;
        char val[128]; val[0]=0;
        if(*p=='='){
            p++;
            while(axmui_is_space(*p)) p++;
            char quote=0;
            if(*p=='"'||*p=='\''){ quote=*p; p++; const char *vs=p;
                while(*p && *p!=quote) p++; size_t vlen=(size_t)(p - vs);
                if(vlen>=sizeof(val)) vlen=sizeof(val)-1; memcpy(val,vs,vlen); val[vlen]=0;
                if(*p==quote) p++;
            } else {
                const char *vs=p; while(*p && !axmui_is_space(*p) && *p!='>' && *p!='/') p++; size_t vlen=(size_t)(p - vs);
                if(vlen>=sizeof(val)) vlen=sizeof(val)-1; memcpy(val,vs,vlen); val[vlen]=0;
            }
        }
        if(axmui_str_case_eq(nbuf,"id")){ strncpy(node->id,val,sizeof(node->id)-1); }
        else if(axmui_str_case_eq(nbuf,"class")){ strncpy(node->klass,val,sizeof(node->klass)-1); }
        else if(axmui_str_case_eq(nbuf,"style")){ strncpy(node->inline_style,val,sizeof(node->inline_style)-1); }
        else if(axmui_str_case_eq(nbuf,"placeholder")){ strncpy(node->placeholder,val,sizeof(node->placeholder)-1); }
        else if(axmui_str_case_eq(nbuf,"value")){ strncpy(node->value,val,sizeof(node->value)-1); strncpy(node->text,val,sizeof(node->text)-1); }
        else if(axmui_str_case_eq(nbuf,"type")){
            if(axmui_str_case_eq(val,"password")) node->input_secure=1;
            else if(axmui_str_case_eq(val,"checkbox")){ node->is_checkbox=1; }
        }
        else if(axmui_str_case_eq(nbuf,"checked")){ node->checked=1; }
    }
}

static axmui_view_t *axmui_parse_html_internal(axmui_window_t *win, const char *html, axmui_view_t *root){
    const char *p=html;
    axmui_view_t *stack[32]; int sp=0;
    if(root){ stack[sp++]=root; }
    while(*p){
        if(*p=='<'){
            if(p[1]=='!' && p[2]=='-' && p[3]=='-'){
                const char *e=strstr(p,"-->"); if(!e) break; p=e+3; continue;
            }
            if(p[1]=='/'){
                const char *gt=strchr(p,'>'); if(!gt) break;
                if(sp>1) sp--;
                p=gt+1; continue;
            }
            /* open tag */
            const char *gt=strchr(p,'>'); if(!gt) break;
            size_t tlen=(size_t)(gt - p -1);
            char tbuf[256]; if(tlen>=sizeof(tbuf)) tlen=sizeof(tbuf)-1;
            memcpy(tbuf,p+1,tlen); tbuf[tlen]=0;
            /* check self-close */
            int self_close=0;
            /* trim trailing spaces and '/' */
            size_t tl=strlen(tbuf); while(tl>0 && axmui_is_space(tbuf[tl-1])) tl--; tbuf[tl]=0;
            if(tl>0 && tbuf[tl-1]=='/'){ self_close=1; tbuf[tl-1]=0; axmui_trim(tbuf); }
            /* tag name */
            char tag[16]; size_t ti=0;
            const char *tp=tbuf;
            while(*tp && !axmui_is_space(*tp) && *tp!='/') { if(ti+1<sizeof(tag)) tag[ti++]=*tp; tp++; }
            tag[ti]=0;
            if(tag[0]==0){ p=gt+1; continue; }
            axmui_view_t *node=axmui_alloc_node(win);
            if(!node){ p=gt+1; continue; }
            strncpy(node->tag, tag, sizeof(node->tag)-1);
            for(size_t k=0;node->tag[k];k++) if(node->tag[k]>='A'&&node->tag[k]<='Z') node->tag[k]+=32;
            axmui_parse_attrs(tp, node);
            if(axmui_is_void_tag(node->tag)) self_close=1;
            if(sp>0) axmui_append_child(stack[sp-1], node);
            if(!self_close){
                if(sp<32) stack[sp++]=node;
            }
            p=gt+1;
        } else {
            const char *lt=strchr(p,'<'); if(!lt) lt=p+strlen(p);
            size_t tlen=(size_t)(lt - p);
            char tbuf[AXMUI_TEXT_CAP]; if(tlen>=sizeof(tbuf)) tlen=sizeof(tbuf)-1;
            memcpy(tbuf,p,tlen); tbuf[tlen]=0;
            /* trim but keep internal spaces */
            /* if text is all whitespace skip */
            int allws=1; for(size_t i=0;i<tlen;i++) if(!axmui_is_space(tbuf[i])){ allws=0; break; }
            if(!allws && sp>0){
                axmui_view_t *par=stack[sp-1];
                /* if parent has no children with text, put into parent's text; else make text node */
                /* For simplicity create text node */
                if(par->first_child==0 && par->text[0]==0){
                    axmui_trim(tbuf);
                    strncpy(par->text, tbuf, sizeof(par->text)-1);
                } else {
                    axmui_view_t *tn=axmui_alloc_node(win);
                    if(tn){
                        tn->is_text=1;
                        strcpy(tn->tag,"#text");
                        axmui_trim(tbuf);
                        strncpy(tn->text, tbuf, sizeof(tn->text)-1);
                        axmui_append_child(par, tn);
                    }
                }
            }
            p=lt;
        }
    }
    return root;
}

static axmui_view_t *axmui_parse_html(axmui_window_t *win, const char *html){
    if(!win||!html) return 0;
    /* root is already win->root */
    return axmui_parse_html_internal(win, html, win->root);
}

/* ------------------------------------------------------------------ */
/*  Layout engine                                                      */

static int axmui_content_width(axmui_view_t *n, int avail){
    if(n->style.width_auto) return avail;
    if(n->style.width_pct) return avail * n->style.width / 100;
    return n->style.width;
}
static int axmui_intrinsic_width(axmui_view_t *n){
    if(!n) return 0;
    if(n->is_checkbox) return 18;
    if(n->is_text) return (int)strlen(n->text)*FONT_WIDTH;
    if(n->text[0] && !n->first_child){
        int pad = n->style.padding[1]+n->style.padding[3];
        int bw = n->style.border_w*2;
        return (int)strlen(n->text)*FONT_WIDTH + pad + bw;
    }
    if(n->first_child){
        if(n->style.display==AXMUI_DISPLAY_FLEX && n->style.flex_dir==AXMUI_FLEX_ROW){
            int w=0;
            for(axmui_view_t *c=n->first_child;c;c=c->next_sibling){
                if(c->style.display==AXMUI_DISPLAY_NONE||!c->style.visible) continue;
                int cw = c->style.width_auto ? axmui_intrinsic_width(c) : (c->style.width_pct? 0 : c->style.width);
                w += cw + c->style.margin[1]+c->style.margin[3];
            }
            w += n->style.gap * ((n->first_child?1:0));
            w += n->style.padding[1]+n->style.padding[3];
            return w;
        }
        if(n->style.display==AXMUI_DISPLAY_FLEX && n->style.flex_dir==AXMUI_FLEX_COL){
            int maxw=0;
            for(axmui_view_t *c=n->first_child;c;c=c->next_sibling){
                if(c->style.display==AXMUI_DISPLAY_NONE||!c->style.visible) continue;
                int cw = c->style.width_auto ? axmui_intrinsic_width(c) : (c->style.width_pct? 0 : c->style.width);
                if(cw>maxw) maxw=cw;
            }
            return maxw + n->style.padding[1]+n->style.padding[3];
        }
        return 80;
    }
    return 80;
}
static int axmui_intrinsic_height(axmui_view_t *n){
    if(!n) return 0;
    if(n->is_checkbox) return 18;
    if(n->is_text) return FONT_HEIGHT;
    if(n->text[0] && !n->first_child) return FONT_HEIGHT + n->style.padding[0]+n->style.padding[2] + n->style.border_w*2;
    if(axmui_str_case_eq(n->tag,"input") && !n->is_checkbox) return 28;
    if(axmui_str_case_eq(n->tag,"button")) return FONT_HEIGHT + n->style.padding[0]+n->style.padding[2] + n->style.border_w*2;
    if(n->first_child){
        if(n->style.display==AXMUI_DISPLAY_FLEX && n->style.flex_dir==AXMUI_FLEX_ROW){
            int maxh=0;
            for(axmui_view_t *c=n->first_child;c;c=c->next_sibling){
                if(c->style.display==AXMUI_DISPLAY_NONE||!c->style.visible) continue;
                int ch = c->style.height_auto ? axmui_intrinsic_height(c) : (c->style.height_pct? 0 : c->style.height);
                if(ch>maxh) maxh=ch;
            }
            return maxh + n->style.padding[0]+n->style.padding[2];
        }
        if(n->style.display==AXMUI_DISPLAY_FLEX && n->style.flex_dir==AXMUI_FLEX_COL){
            int h=0, cnt=0;
            for(axmui_view_t *c=n->first_child;c;c=c->next_sibling){
                if(c->style.display==AXMUI_DISPLAY_NONE||!c->style.visible) continue;
                int ch = c->style.height_auto ? axmui_intrinsic_height(c) : (c->style.height_pct? 0 : c->style.height);
                h += ch + c->style.margin[0]+c->style.margin[2];
                cnt++;
            }
            if(cnt>1) h += n->style.gap*(cnt-1);
            return h + n->style.padding[0]+n->style.padding[2];
        }
        return FONT_HEIGHT+8;
    }
    return FONT_HEIGHT+8;
}

static void axmui_layout_node(axmui_view_t *node, int avail_w, int avail_h);

static void axmui_layout_children(axmui_view_t *node){
    /* dispatch based on display */
    if(node->style.display==AXMUI_DISPLAY_NONE) return;
    int pad_l=node->style.padding[3], pad_r=node->style.padding[1];
    int pad_t=node->style.padding[0], pad_b=node->style.padding[2];
    int cw = node->w - pad_l - pad_r;
    if(cw<0) cw=0;
    int ch = node->h - pad_t - pad_b;
    if(node->style.display==AXMUI_DISPLAY_FLEX){
        /* flex row/col */
        int is_row = node->style.flex_dir==AXMUI_FLEX_ROW;
        /* collect visible children */
        axmui_view_t *kids[AXMUI_MAX_CHILDREN]; int nk=0;
        for(axmui_view_t *c=node->first_child;c;c=c->next_sibling) if(c->style.display!=AXMUI_DISPLAY_NONE && c->style.visible) kids[nk++]=c;
        if(nk==0) return;
        int gap=node->style.gap;
        int total_gap = gap * (nk>0? nk-1:0);
        if(is_row){
            /* row: primary is x, cross is y — use intrinsic for auto width, respect justify */
            int total_fixed=0, total_flex=0;
            for(int i=0;i<nk;i++){
                axmui_view_t *k=kids[i];
                if(k->style.flex_grow>0) total_flex+=k->style.flex_grow;
                else {
                    int kw;
                    if(k->style.width_auto) kw=axmui_intrinsic_width(k);
                    else kw=axmui_content_width(k, cw);
                    kw += k->style.margin[1]+k->style.margin[3];
                    total_fixed+=kw;
                }
            }
            int flex_space = cw - total_fixed - total_gap;
            if(flex_space<0) flex_space=0;
            int cur=pad_l;
            /* justify offset */
            if(node->style.justify==AXMUI_JUSTIFY_CENTER) cur = pad_l + flex_space/2;
            else if(node->style.justify==AXMUI_JUSTIFY_END) cur = pad_l + flex_space;
            else if(node->style.justify==AXMUI_JUSTIFY_BETWEEN && nk>1) gap += flex_space/(nk-1);
            else if(node->style.justify==AXMUI_JUSTIFY_AROUND) cur = pad_l + flex_space/(nk*2);
            for(int i=0;i<nk;i++){
                axmui_view_t *k=kids[i];
                int kw;
                if(k->style.flex_grow>0 && total_flex>0){
                    kw = flex_space * k->style.flex_grow / total_flex;
                    if(k->style.width_auto){
                        int intr=axmui_intrinsic_width(k);
                        if(kw < intr) kw=intr;
                    }
                } else {
                    if(k->style.width_auto) kw=axmui_intrinsic_width(k);
                    else kw=axmui_content_width(k, cw);
                }
                int kh;
                if(k->style.height_auto){
                    if(node->style.align==AXMUI_ALIGN_STRETCH) kh=ch - k->style.margin[0]-k->style.margin[2];
                    else kh=axmui_intrinsic_height(k);
                } else if(k->style.height_pct) kh = ch * k->style.height /100;
                else kh=k->style.height;
                k->w=kw;
                k->h=kh;
                if(k->w<0) k->w=0; if(k->h<0) k->h=0;
                /* add margins to layout offset */
                k->x = cur + k->style.margin[3];
                /* cross alignment */
                if(node->style.align==AXMUI_ALIGN_CENTER) k->y = pad_t + (ch - k->h)/2;
                else if(node->style.align==AXMUI_ALIGN_END) k->y = pad_t + ch - k->h - k->style.margin[2];
                else if(node->style.align==AXMUI_ALIGN_START) k->y = pad_t + k->style.margin[0];
                else k->y = pad_t + k->style.margin[0];
                cur += kw + k->style.margin[1]+k->style.margin[3] + gap;
                /* recurse */
                axmui_layout_node(k, k->w, k->h);
            }
        } else {
            /* column */
            int total_fixed=0, total_flex=0;
            for(int i=0;i<nk;i++){
                axmui_view_t *k=kids[i];
                if(k->style.flex_grow>0) total_flex+=k->style.flex_grow;
                else {
                    int kh;
                    if(k->style.height_auto) kh=axmui_intrinsic_height(k);
                    else if(k->style.height_pct) kh=ch*k->style.height/100;
                    else kh=k->style.height;
                    kh += k->style.margin[0]+k->style.margin[2];
                    total_fixed+=kh;
                }
            }
            int flex_space = ch - total_fixed - total_gap;
            if(flex_space<0) flex_space=0;
            int cur=pad_t;
            for(int i=0;i<nk;i++){
                axmui_view_t *k=kids[i];
                int kw;
                if(k->style.width_auto) kw = cw - k->style.margin[1]-k->style.margin[3];
                else if(k->style.width_pct) kw = cw * k->style.width/100;
                else kw=k->style.width;
                int kh;
                if(k->style.flex_grow>0 && total_flex>0){
                    kh = flex_space * k->style.flex_grow / total_flex;
                } else {
                    if(k->style.height_auto) kh=axmui_intrinsic_height(k);
                    else if(k->style.height_pct) kh=ch*k->style.height/100;
                    else kh=k->style.height;
                }
                k->w=kw; if(k->w<0) k->w=0; if(k->w>cw) k->w=cw;
                k->h=kh; if(k->h<0) k->h=0;
                k->x = pad_l + k->style.margin[3];
                if(node->style.align==AXMUI_ALIGN_CENTER) k->x = pad_l + (cw - k->w)/2;
                else if(node->style.align==AXMUI_ALIGN_END) k->x = pad_l + cw - k->w - k->style.margin[1];
                k->y = cur + k->style.margin[0];
                cur += k->h + k->style.margin[0]+k->style.margin[2] + gap;
                axmui_layout_node(k, k->w, k->h);
                /* after child layout, if height_auto, adjust */
                if(k->style.height_auto){
                    /* use child's computed needed height: if has children, use aggregated */
                    /* for leaf text, height is font */
                    /* keep k->h as at least computed */
                }
            }
            /* if node height auto, shrink/expand to fit content */
            if(node->style.height_auto){
                int needed = cur - pad_t + pad_b - gap; /* remove trailing gap? already gap not after last, our cur includes gap after each, we added gap each time */
                /* cur currently points after last child + gap, correct: subtract last gap */
                if(nk>0) needed -= gap;
                if(needed<0) needed=0;
                node->h = needed + pad_t + pad_b;
                /* respect min/max if needed */
            }
        }
    } else {
        /* block: vertical stack */
        int cur=pad_t;
        int gap=node->style.gap;
        for(axmui_view_t *c=node->first_child;c;c=c->next_sibling){
            if(c->style.display==AXMUI_DISPLAY_NONE || !c->style.visible) continue;
            int cw_child = cw - c->style.margin[1]-c->style.margin[3];
            if(!c->style.width_auto){
                if(c->style.width_pct) c->w = cw * c->style.width/100;
                else c->w = c->style.width;
                if(c->w>cw_child) c->w=cw_child;
            } else {
                c->w=cw_child;
            }
            if(c->style.height_auto) c->h=0; /* will compute */
            else if(c->style.height_pct) c->h = ch * c->style.height/100;
            else c->h=c->style.height;
            if(c->w<0) c->w=0; if(c->h<0) c->h=0;
            c->x = pad_l + c->style.margin[3];
            c->y = cur + c->style.margin[0];
            axmui_layout_node(c, c->w, c->h>0?c->h: 1000);
            if(c->style.height_auto){
                /* after layout, if child has intrinsic height, use it */
                if(c->h==0) c->h=FONT_HEIGHT + c->style.padding[0]+c->style.padding[2];
            }
            cur += c->h + c->style.margin[0]+c->style.margin[2] + gap;
        }
        if(node->style.height_auto){
            int needed = cur - pad_t;
            if(node->first_child) needed -= gap; /* remove last gap */
            node->h = needed + pad_t + pad_b;
        }
    }
}

static void axmui_layout_node(axmui_view_t *node, int avail_w, int avail_h){
    if(!node) return;
    if(node->is_text){
        node->w = (int)strlen(node->text) * FONT_WIDTH;
        node->h = FONT_HEIGHT;
        return;
    }
    int pad_l=node->style.padding[3], pad_r=node->style.padding[1];
    int pad_t=node->style.padding[0], pad_b=node->style.padding[2];
    /* compute own size if not already set by parent flex */
    if(node->w==0 && avail_w>0){
        if(node->style.width_auto) node->w=avail_w;
        else if(node->style.width_pct) node->w=avail_w * node->style.width/100;
        else node->w=node->style.width;
    }
    if(node->h==0 && avail_h>0){
        if(node->style.height_auto){
            /* will be computed from children later; give spacious */
            node->h=avail_h;
        } else if(node->style.height_pct) node->h=avail_h * node->style.height/100;
        else node->h=node->style.height;
    }
    /* If leaf with text but no children: size to text */
    if(!node->first_child && node->text[0]){
        int tw = (int)strlen(node->text)*FONT_WIDTH + pad_l+pad_r;
        int th = FONT_HEIGHT + pad_t+pad_b;
        if(node->style.width_auto && tw>node->w) node->w=tw;
        if(node->style.height_auto) node->h=th;
    }
    /* special for input: keep height */
    axmui_layout_children(node);
    /* for text-only nodes without explicit size, ensure h */
    if(node->style.height_auto && node->first_child==0 && node->h< (FONT_HEIGHT+pad_t+pad_b)){
        if(node->text[0]) node->h=FONT_HEIGHT+pad_t+pad_b;
    }
}

static void axmui_layout(axmui_view_t *root, int avail_w, int avail_h){
    if(!root) return;
    root->x=0; root->y=0; root->w=avail_w; root->h=avail_h;
    axmui_layout_node(root, avail_w, avail_h);
    /* compute absolute positions */
    /* iterative stack */
    axmui_view_t *stack[AXMUI_MAX_NODES]; int sp=0;
    root->abs_x=root->x; root->abs_y=root->y;
    stack[sp++]=root;
    while(sp>0){
        axmui_view_t *n=stack[--sp];
        for(axmui_view_t *c=n->first_child;c;c=c->next_sibling){
            c->abs_x = n->abs_x + c->x;
            c->abs_y = n->abs_y + c->y;
            if(sp<AXMUI_MAX_NODES) stack[sp++]=c;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Render helpers                                                     */

static void axmui_fill_rect(struct axgui_fb *fb, int x,int y,int w,int h, uint32_t rgb){
    axgui_fill(fb,x,y,w,h,rgb);
}
static void axmui_draw_rect(struct axgui_fb *fb, int x,int y,int w,int h, uint32_t rgb){
    axgui_rect(fb,x,y,w,h,rgb);
}

static void axmui_fill_rounded(struct axgui_fb *fb, int x,int y,int w,int h,int r, uint32_t rgb){
    if(r<=0 || w<=0 || h<=0){ axmui_fill_rect(fb,x,y,w,h,rgb); return; }
    if(r*2 > w) r=w/2;
    if(r*2 > h) r=h/2;
    /* central rects */
    axmui_fill_rect(fb, x+r, y, w-2*r, h, rgb);
    axmui_fill_rect(fb, x, y+r, r, h-2*r, rgb);
    axmui_fill_rect(fb, x+w-r, y+r, r, h-2*r, rgb);
    /* corners as quarter circles brute */
    for(int dy=0; dy<r; dy++){
        for(int dx=0; dx<r; dx++){
            int dist2 = (r-1-dx)*(r-1-dx) + (r-1-dy)*(r-1-dy);
            if(dist2 > r*r) continue;
            /* top-left */
            axgui_px(fb, x+dx, y+dy, rgb);
            /* top-right */
            axgui_px(fb, x+w-1-dx, y+dy, rgb);
            /* bottom-left */
            axgui_px(fb, x+dx, y+h-1-dy, rgb);
            /* bottom-right */
            axgui_px(fb, x+w-1-dx, y+h-1-dy, rgb);
        }
    }
}

static void axmui_stroke_rounded(struct axgui_fb *fb, int x,int y,int w,int h,int r, int bw, uint32_t col){
    if(bw<=0) return;
    if(r<=0){ axmui_draw_rect(fb,x,y,w,h,col); return; }
    /* stroke outer, inner clip not perfect but looks fine: draw outer rounded then inner transparent */
    axmui_fill_rounded(fb,x,y,w,h,r,col);
    /* inner fill with transparent would show bg; instead caller draws bg then stroke with inner cut -> we just draw border as outer - inner */
    /* simplest: draw border as 4 rects approximation (acceptable for 1px) */
    /* For bw==1 outer already looks okay; for thicker we inner */
    if(bw==1) return;
    /* inner rect erases interior (caller will have drawn bg underneath, so we re-fill interior with bg color? We can't know bg — caller handles) */
}

static uint32_t axmui_effective_bg(axmui_view_t *n){
    while(n){
        if(n->style.bg != 0xFFFFFFFFu) return n->style.bg;
        n=n->parent;
    }
    return 0x0F1722u;
}
static void axmui_render_text(struct axgui_fb *fb, axmui_view_t *node){
    if(!node->text[0]) return;
    int tx = node->abs_x + node->style.padding[3];
    int ty = node->abs_y + node->style.padding[0];
    /* text align */
    int content_w = node->w - node->style.padding[1]-node->style.padding[3];
    int tw = (int)strlen(node->text)*FONT_WIDTH;
    if(node->style.text_align==AXMUI_TEXT_CENTER) tx = node->abs_x + (content_w - tw)/2 + node->style.padding[3];
    else if(node->style.text_align==AXMUI_TEXT_RIGHT) tx = node->abs_x + content_w - tw + node->style.padding[3];
    uint32_t bg = axmui_effective_bg(node);
    axgui_text(fb, node->text, tx, ty, node->style.fg, bg);
}

static void axmui_render_node(struct axgui_fb *fb, axmui_view_t *node){
    if(!node || node->style.display==AXMUI_DISPLAY_NONE || !node->style.visible) return;
    if(node->is_text){
        int tx=node->abs_x;
        int ty=node->abs_y;
        uint32_t fg=node->parent? node->parent->style.fg : 0xD8DEE9u;
        uint32_t bg=axmui_effective_bg(node->parent);
        axgui_text(fb, node->text, tx, ty, fg, bg);
        return;
    }
    int x=node->abs_x;
    int y=node->abs_y;
    int w=node->w;
    int h=node->h;
    if(w<=0||h<=0) return;
    int r=node->style.radius;
    uint32_t bg=node->style.bg;
    uint32_t bc=node->style.border_color;
    int bw=node->style.border_w;

    if(node->custom_draw){
        if(bg!=0xFFFFFFFFu){
            if(r>0) axmui_fill_rounded(fb,x,y,w,h,r,bg);
            else axmui_fill_rect(fb,x,y,w,h,bg);
        }
        if(bw>0){
            if(r>0){
                axmui_fill_rounded(fb,x,y,w,h,r,bc);
                if(bg!=0xFFFFFFFFu){
                    axmui_fill_rounded(fb, x+bw, y+bw, w-2*bw, h-2*bw, r>bw? r-bw:0, bg);
                }
            } else {
                axgui_rect(fb,x,y,w,h,bc);
                if(bw>1) for(int i=1;i<bw;i++) axgui_rect(fb,x+i,y+i,w-2*i,h-2*i,bc);
            }
        }
        node->custom_draw(fb, node, node->custom_data);
    } else {
        if(bg!=0xFFFFFFFFu){
            if(r>0) axmui_fill_rounded(fb,x,y,w,h,r,bg);
            else axmui_fill_rect(fb,x,y,w,h,bg);
        }
        /* border */
        if(bw>0){
            if(r>0){
                /* for rounded we already have outer, now just stroke: draw outer again as border then inner bg already? simpler */
                /* draw border by outer rounded in border color, then inner rounded slightly smaller with bg */
                /* Instead we re-draw outer then inner bg */
                axmui_fill_rounded(fb,x,y,w,h,r,bc);
                if(bg!=0xFFFFFFFFu){
                    axmui_fill_rounded(fb, x+bw, y+bw, w-2*bw, h-2*bw, r>bw? r-bw:0, bg);
                }
            } else {
                axgui_rect(fb,x,y,w,h,bc);
                if(bw>1){
                    /* inner rects for thick */
                    for(int i=1;i<bw;i++) axgui_rect(fb,x+i,y+i,w-2*i,h-2*i,bc);
                }
            }
        }
        if(node->is_checkbox){
            if(node->checked){
                uint32_t tick=0xFFFFFFu;
                /* small checkmark inside 18x18 box */
                int cx=x+4, cy=y+9;
                (void)cx; (void)cy;
                axgui_px(fb, x+4, y+9, tick); axgui_px(fb, x+5, y+10, tick);
                axgui_px(fb, x+6, y+11, tick); axgui_px(fb, x+7, y+12, tick);
                axgui_px(fb, x+8, y+11, tick); axgui_px(fb, x+9, y+10, tick);
                axgui_px(fb, x+10, y+9, tick); axgui_px(fb, x+11, y+8, tick);
                /* thicker */
                axgui_px(fb, x+4, y+10, tick); axgui_px(fb, x+5, y+11, tick);
                axgui_px(fb, x+6, y+12, tick); axgui_px(fb, x+7, y+13, tick);
                axgui_px(fb, x+8, y+12, tick); axgui_px(fb, x+9, y+11, tick);
                axgui_px(fb, x+10, y+10, tick); axgui_px(fb, x+11, y+9, tick);
            }
        } else if(node->text[0] && !node->first_child){
            /* special handling for button/input alignment */
            if(axmui_str_case_eq(node->tag,"button")||(axmui_str_case_eq(node->tag,"input")&&!node->is_checkbox)){
                /* center or left */
                char disp[AXMUI_TEXT_CAP];
                if(node->input_secure && node->value[0]){
                    size_t l=strlen(node->value); if(l>=sizeof(disp)) l=sizeof(disp)-1;
                    for(size_t i=0;i<l;i++) disp[i]='*'; disp[l]=0;
                    /* for input secure show stars + placeholder fallback */
                } else if(axmui_str_case_eq(node->tag,"input") && node->value[0]){
                    strncpy(disp, node->value, sizeof(disp)-1); disp[sizeof(disp)-1]=0;
                } else if(node->text[0]){
                    strncpy(disp, node->text, sizeof(disp)-1); disp[sizeof(disp)-1]=0;
                } else if(node->placeholder[0]){
                    strncpy(disp, node->placeholder, sizeof(disp)-1); disp[sizeof(disp)-1]=0;
                } else disp[0]=0;
                if(disp[0]){
                    int pad_l=node->style.padding[3], pad_t=node->style.padding[0];
                    int content_w=w - node->style.padding[1]-pad_l;
                    int tw=(int)strlen(disp)*FONT_WIDTH;
                    int tx=x+pad_l, ty=y+pad_t;
                    if(axmui_str_case_eq(node->tag,"button")){
                        tx = x + (w - tw)/2;
                        ty = y + (h - FONT_HEIGHT)/2;
                    }
                    uint32_t tbg = axmui_effective_bg(node);
                    uint32_t tfg = node->style.fg;
                    if(axmui_str_case_eq(node->tag,"input") && node->value[0]==0 && node->placeholder[0]){
                        tfg=0x6B7280u;
                    }
                    axgui_text(fb, disp, tx, ty, tfg, tbg);
                    if(node->focused && axmui_str_case_eq(node->tag,"input")){
                        int cx = tx + (int)strlen(disp)*FONT_WIDTH;
                        if(cx+2 < x+w) axmui_fill_rect(fb, cx, ty, 2, FONT_HEIGHT, node->style.fg);
                    }
                }
            } else {
                axmui_render_text(fb, node);
            }
        } else if(node->text[0] && node->first_child){
            /* mixed: draw own text at top-left padding before children already laid out? children cover area, so skip */
        }
    }
    for(axmui_view_t *c=node->first_child;c;c=c->next_sibling) axmui_render_node(fb,c);
}

/* ------------------------------------------------------------------ */
/*  Hit test + focus management                                        */

static axmui_view_t *axmui_hit_test(axmui_view_t *root, int x,int y){
    axmui_view_t *hit=0;
    if(!root) return 0;
    if(x < root->abs_x || y < root->abs_y || x >= root->abs_x + root->w || y >= root->abs_y + root->h) return 0;
    /* deepest */
    for(axmui_view_t *c=root->first_child;c;c=c->next_sibling){
        axmui_view_t *h=axmui_hit_test(c,x,y);
        if(h) hit=h;
    }
    if(hit) return hit;
    if(root->style.display==AXMUI_DISPLAY_NONE) return 0;
    /* only interactive or box hits */
    return root;
}

static void axmui_clear_pseudo(axmui_view_t *root){
    if(!root) return;
    root->hovered=0; root->active=0;
    for(axmui_view_t *c=root->first_child;c;c=c->next_sibling) axmui_clear_pseudo(c);
}

static int axmui_handle_mouse(axmui_window_t *win, int x, int y, uint32_t code, uint32_t *prev_btn){
    if(!win||!prev_btn) return 0;
    axmui_view_t *hit=axmui_hit_test(win->root, x, y);
    axmui_clear_pseudo(win->root);
    if(hit){ for(axmui_view_t *p=hit;p;p=p->parent) p->hovered=1; win->hovered=hit; }
    if(code & AXINPUT_BTN_LEFT){ if(hit) hit->active=1; win->active=hit; }
    uint32_t pressed = code & ~*prev_btn;
    uint32_t released = ~code & *prev_btn;
    if(pressed & AXINPUT_BTN_LEFT){
        if(hit && hit->is_checkbox){
            hit->checked=!hit->checked;
            win->focused=hit;
            for(axmui_view_t *n=win->root; n; ){
                n->focused=(n==hit);
                if(n->first_child){ n=n->first_child; continue; }
                while(n && !n->next_sibling) n=n->parent;
                if(n) n=n->next_sibling; else break;
            }
        } else if(hit && axmui_str_case_eq(hit->tag,"label") && hit->parent){
            axmui_view_t *cb=NULL;
            for(axmui_view_t *c=hit->parent->first_child;c;c=c->next_sibling) if(c->is_checkbox){ cb=c; break; }
            if(cb){
                cb->checked=!cb->checked;
                win->focused=cb;
                for(axmui_view_t *n=win->root; n; ){
                    n->focused=(n==cb);
                    if(n->first_child){ n=n->first_child; continue; }
                    while(n && !n->next_sibling) n=n->parent;
                    if(n) n=n->next_sibling; else break;
                }
                hit=cb;
            }
        } else if(hit && axmui_str_case_eq(hit->tag,"input")){
            win->focused=hit;
            for(axmui_view_t *n=win->root; n; ){
                n->focused=(n==hit);
                if(n->first_child){ n=n->first_child; continue; }
                while(n && !n->next_sibling) n=n->parent;
                if(n) n=n->next_sibling; else break;
            }
        } else if(hit && axmui_str_case_eq(hit->tag,"button")){
            win->focused=hit;
            for(axmui_view_t *n=win->root; n; ){
                n->focused=(n==hit);
                if(n->first_child){ n=n->first_child; continue; }
                while(n && !n->next_sibling) n=n->parent;
                if(n) n=n->next_sibling; else break;
            }
        }
        if(hit && hit->on_click) hit->on_click(hit, hit->on_click_data);
        else if(hit && hit->parent){
            /* if label was hit and checkbox was toggled, fire checkbox handler */
            for(axmui_view_t *c=hit->parent->first_child;c;c=c->next_sibling) if(c->is_checkbox && c->on_click){ c->on_click(c,c->on_click_data); break; }
        }
    }
    if(released & AXINPUT_BTN_LEFT){
        if(win->active) win->active->active=0;
        win->active=NULL;
    }
    *prev_btn=code;
    return hit?1:0;
}

/* ------------------------------------------------------------------ */
/*  Public view helpers (Cocoa/GTK-like)                               */

static axmui_view_t *axmui_view_create(axmui_window_t *win, const char *tag, const char *klass){
    axmui_view_t *n=axmui_alloc_node(win);
    if(!n) return 0;
    if(tag) strncpy(n->tag, tag, sizeof(n->tag)-1);
    if(klass) strncpy(n->klass, klass, sizeof(n->klass)-1);
    return n;
}
static axmui_view_t *axmui_label_create(axmui_window_t *win, const char *text){
    axmui_view_t *n=axmui_view_create(win,"label",0);
    if(n && text) strncpy(n->text,text,sizeof(n->text)-1);
    return n;
}
static axmui_view_t *axmui_button_create(axmui_window_t *win, const char *title){
    axmui_view_t *n=axmui_view_create(win,"button",0);
    if(n && title) strncpy(n->text,title,sizeof(n->text)-1);
    return n;
}
static axmui_view_t *axmui_input_create(axmui_window_t *win, const char *placeholder, int secure){
    axmui_view_t *n=axmui_view_create(win,"input",0);
    if(!n) return 0;
    if(placeholder) strncpy(n->placeholder, placeholder, sizeof(n->placeholder)-1);
    n->input_secure=secure;
    return n;
}
static axmui_view_t *axmui_container_create(axmui_window_t *win, const char *klass){
    return axmui_view_create(win,"div",klass);
}

static void axmui_view_append(axmui_view_t *parent, axmui_view_t *child){
    axmui_append_child(parent,child);
}
static void axmui_view_remove(axmui_view_t *parent, axmui_view_t *child){
    if(!parent||!child||child->parent!=parent) return;
    if(child->prev_sibling) child->prev_sibling->next_sibling=child->next_sibling;
    else parent->first_child=child->next_sibling;
    if(child->next_sibling) child->next_sibling->prev_sibling=child->prev_sibling;
    else parent->last_child=child->prev_sibling;
    child->parent=0; child->next_sibling=child->prev_sibling=0;
}
static axmui_view_t *axmui_view_find(axmui_view_t *root, const char *id){
    if(!root||!id) return 0;
    if(axmui_str_case_eq(root->id,id)) return root;
    for(axmui_view_t *c=root->first_child;c;c=c->next_sibling){
        axmui_view_t *f=axmui_view_find(c,id); if(f) return f;
    }
    return 0;
}
static void axmui_view_set_text(axmui_view_t *v, const char *text){
    if(!v||!text) return;
    strncpy(v->text,text,sizeof(v->text)-1);
    strncpy(v->value,text,sizeof(v->value)-1);
}
static void axmui_view_set_attr(axmui_view_t *v, const char *name, const char *value){
    if(!v||!name||!value) return;
    if(axmui_str_case_eq(name,"id")) strncpy(v->id,value,sizeof(v->id)-1);
    else if(axmui_str_case_eq(name,"class")) strncpy(v->klass,value,sizeof(v->klass)-1);
    else if(axmui_str_case_eq(name,"placeholder")) strncpy(v->placeholder,value,sizeof(v->placeholder)-1);
    else if(axmui_str_case_eq(name,"value")){ strncpy(v->value,value,sizeof(v->value)-1); strncpy(v->text,value,sizeof(v->text)-1); }
}
static void axmui_view_set_style(axmui_view_t *v, const char *prop, const char *value){
    if(!v||!prop||!value) return;
    char tmp[64]; strncpy(tmp,value,63); tmp[63]=0;
    /* apply directly to computed; inline style also appended for persistence */
    axmui_apply_decl(&v->style, prop, value);
    /* append to inline_style for recompute */
    size_t cur=strlen(v->inline_style);
    if(cur+strlen(prop)+strlen(value)+4 < sizeof(v->inline_style)){
        if(cur>0){ v->inline_style[cur++]=';'; v->inline_style[cur]=' '; }
        strcpy(v->inline_style+cur, prop); cur+=strlen(prop);
        strcpy(v->inline_style+cur, ":"); cur+=1;
        strcpy(v->inline_style+cur, value); cur+=strlen(value);
        strcpy(v->inline_style+cur, ";");
    }
}
static void axmui_view_set_class(axmui_view_t *v, const char *cls){ if(v&&cls) strncpy(v->klass,cls,sizeof(v->klass)-1); }
static void axmui_view_set_id(axmui_view_t *v, const char *id){ if(v&&id) strncpy(v->id,id,sizeof(v->id)-1); }
static void axmui_view_on_click(axmui_view_t *v, axmui_callback_t cb, void *ud){ if(v){ v->on_click=cb; v->on_click_data=ud; } }
static void axmui_view_on_input(axmui_view_t *v, axmui_callback_t cb, void *ud){ if(v){ v->on_input=cb; v->on_input_data=ud; } }
static void axmui_view_set_draw(axmui_view_t *v, axmui_draw_fn_t fn, void *ud){ if(v){ v->custom_draw=fn; v->custom_data=ud; } }
static const char *axmui_view_get_value(axmui_view_t *v){ return v? v->value : ""; }
static const char *axmui_view_get_text(axmui_view_t *v){ return v? v->text : ""; }

/* ------------------------------------------------------------------ */
/*  Window / App lifecycle                                             */

static void axmui_window_init_default_styles(axmui_window_t *win){
    /* UA sheet embedded */
    const char *ua =
        "*{ margin:0; padding:0; }"
        " .window{ background:#0F1722; padding:16px; display:flex; flex-direction:column; gap:12px; }"
        " .card{ background:#1E2A3A; border:1px solid #2B3B55; border-radius:12px; padding:16px; display:flex; flex-direction:column; gap:10px; }"
        " .card-hover:hover{ background:#243447; }"
        " h1{ color:#E2E8F0; font-weight:bold; }"
        " h2{ color:#CBD5E1; }"
        " p{ color:#94A3B8; }"
        " label{ color:#E2E8F0; }"
        " .btn-primary{ background:#3B82F6; border-color:#2563EB; color:white; border-radius:8px; padding:8px 16px; }"
        " .btn-primary:hover{ background:#2563EB; }"
        " .btn-primary:active{ background:#1D4ED8; }"
        " .btn-danger{ background:#EF4444; border-color:#DC2626; color:white; }"
        " .btn-secondary{ background:#334155; border-color:#475569; color:#E2E8F0; }"
        " input{ background:#0B0F14; border:1px solid #334155; border-radius:8px; padding:8px 12px; color:#E2E8F0; }"
        " input:focus{ border-color:#3B82F6; border-width:2px; }"
        " .header{ display:flex; flex-direction:row; justify-content:space-between; align-items:center; padding:8px 0; }"
        " .row{ display:flex; flex-direction:row; gap:12px; align-items:center; }"
        " .col{ display:flex; flex-direction:column; gap:12px; }"
        " .center{ display:flex; justify-content:center; align-items:center; }"
        " .muted{ color:#64748B; }"
        " .title{ color:#F1F5F9; font-weight:bold; text-align:center; }";
    axmui_css_parse(&win->sheet, ua);
}

static axmui_window_t *axmui_window_create(axmui_app_t *app, const char *title, int w, int h){
    (void)w; (void)h;
    if(!app) return 0;
    /* allocate window struct via malloc (one per process) */
    axmui_window_t *win=(axmui_window_t*)malloc(sizeof(*win));
    if(!win) return 0;
    memset(win,0,sizeof(*win));
    if(title) strncpy(win->title,title,sizeof(win->title)-1);
    axmui_sheet_init(&win->sheet);
    axmui_window_init_default_styles(win);
    win->pool_used=0;
    /* root view: fills window */
    win->root=axmui_alloc_node(win);
    if(!win->root){ free(win); return 0; }
    strcpy(win->root->tag,"div");
    strcpy(win->root->klass,"window");
    win->root->style.display=AXMUI_DISPLAY_FLEX;
    win->root->style.flex_dir=AXMUI_FLEX_COL;
    app->win=win;
    return win;
}

static void axmui_window_set_html(axmui_window_t *win, const char *html, const char *css){
    if(!win||!html) return;
    /* reset pool keeping root */
    axmui_view_t *root=win->root;
    /* keep root, discard children */
    root->first_child=root->last_child=0;
    root->text[0]=0;
    /* wipe pool after root (1 node) - re-create children via alloc */
    win->pool_used=1; /* root is pool[0] */
    /* if custom css, parse and append */
    if(css) axmui_css_parse(&win->sheet, css);
    axmui_parse_html(win, html);
    win->needs_layout=1;
    win->needs_render=1;
}

static axmui_view_t *axmui_window_root(axmui_window_t *win){ return win? win->root:0; }
static void axmui_window_on_key(axmui_window_t *win, axmui_callback_t cb, void *ud){ if(win){ win->on_key=cb; win->on_key_data=ud; } }

/* attach to guixd window SHM and start loop; returns when window closed */
static int axmui_window_attach(axmui_window_t *win, struct axclient *cx){
    if(!win||!cx) return -1;
    win->cx=*cx;
    win->fb=cx->fb;
    /* adopt FB dimensions: root covers full window content */
    win->root->w = (int)cx->fb.w;
    win->root->h = (int)cx->fb.h;
    return 0;
}

/* render + commit */
static void axmui_window_render(axmui_window_t *win){
    if(!win||!win->root) return;
    /* compute styles for current pseudo states */
    axmui_compute_styles(win->root, &win->sheet);
    axmui_layout(win->root, win->fb.w, win->fb.h);
    /* clear FB with window bg */
    uint32_t clear = 0x0F1722u;
    if(win->root->style.bg!=0xFFFFFFFFu) clear=win->root->style.bg;
    axmui_fill_rect(&win->fb, 0,0, win->fb.w, win->fb.h, clear);
    axmui_render_node(&win->fb, win->root);
    /* seqlock publish already handled by caller via axclient_begin/commit */
}

/* simple UTF helpers for input */
static void axmui_input_append(axmui_view_t *v, char c){
    size_t l=strlen(v->value);
    if(l+1<sizeof(v->value)){
        v->value[l]=c; v->value[l+1]=0;
        strncpy(v->text, v->value, sizeof(v->text)-1);
    }
}
static void axmui_input_backspace(axmui_view_t *v){
    size_t l=strlen(v->value);
    if(l>0){ v->value[l-1]=0; strncpy(v->text, v->value, sizeof(v->text)-1); }
}

/* ------------------------------------------------------------------ */
/*  App                                                                */

static axmui_app_t *axmui_app_create(void){
    axmui_app_t *a=(axmui_app_t*)malloc(sizeof(*a));
    if(!a) return 0;
    memset(a,0,sizeof(*a));
    a->running=1;
    return a;
}
static void axmui_app_destroy(axmui_app_t *a){ if(a){ if(a->win) free(a->win); free(a); } }

/* High-level run loop that hides axclient boilerplate.
 * Each app process is launched as `prog --wm <shmid> <w> <h> <evfd> <gen> ...`.
 * This mirrors axclient_init: validates argv, attaches SHM, then enters the
 * event loop. For non-window contexts (tests), create window manually.
 */
static int axmui_app_run(axmui_app_t *app, int argc, char **argv, const char *prog){
    struct axclient cx;
    int rest;
    if(!app||!prog) return 1;
    if(!app->win){
        app->win=axmui_window_create(app, prog, WM_WIN_W, WM_WIN_H);
        if(!app->win) return 1;
    }
    rest=axclient_init(&cx, argc, argv, prog);
    if(rest<0) return 1;
    axmui_window_attach(app->win, &cx);
    axmui_window_t *win=app->win;

    /* initial render */
    axclient_begin(&cx);
    axmui_window_render(win);
    cx.hdr->ready=1;
    axclient_commit(&cx);

    uint32_t prev_btn=0;
    while(app->running){
        struct wm_event ev;
        if(axclient_closed(&cx) || axclient_stale(&cx)) break;
        /* poll with blocking recv but with timeout: use nonblocking poll + sleep */
        /* We do a blocking recv with short busy: try poll first */
        /* Use wm_recv in nonblocking style: set fd to nonblock? axclient_recv blocks.
           For axmui we want to handle hover/timeouts, so we use try + msleep.
           Simplify: use a helper that tries nonblock via read polling is not available.
           Instead we use axgui_poll style but for wm pipe we have to use blocking with timeout via fork? easiest: use blocking recv in dedicated pump? 
           Here we implement poll via read with O_NONBLOCK check: we peek if data available via read with 0 timeout by trying recv with MSG_DONTWAIT not available.
           Fallback: block on recv but render on each event – responsiveness still okay because hover needs move events which will arrive.
           So we block on recv; idle redraw not needed except focus blink.
        */
        if(axgui_wm_recv(cx.evfd, &ev) < 0) break;
        if(axclient_closed(&cx) || axclient_stale(&cx)) break;
        if(ev.type==WM_EV_MOUSE){
            axmui_handle_mouse(win, ev.x, ev.y, ev.code, &prev_btn);
            win->needs_render=1;
        } else if(ev.type==WM_EV_KEY){
            /* Tab / Up/Down cycles focus */
            if(ev.code=='\t' || ev.code==AXINPUT_KEY_DOWN || ev.code==AXINPUT_KEY_UP){
                int dir = (ev.code==AXINPUT_KEY_UP) ? -1 : 1;
                axmui_view_t *list[AXMUI_MAX_NODES]; int n=0;
                for(int k=0;k<win->pool_used;k++){
                    axmui_view_t *nd=&win->pool[k];
                    if((axmui_str_case_eq(nd->tag,"input")||axmui_str_case_eq(nd->tag,"button")) && nd->style.display!=AXMUI_DISPLAY_NONE)
                        list[n++]=nd;
                }
                if(n>0){
                    int idx=-1;
                    for(int k=0;k<n;k++) if(list[k]==win->focused) idx=k;
                    int nxt = (idx+dir+n)%n;
                    win->focused=list[nxt];
                    /* clear focused flags */
                    for(int k=0;k<win->pool_used;k++) win->pool[k].focused=0;
                    list[nxt]->focused=1;
                    win->needs_render=1;
                }
            } else {
                axmui_view_t *f=win->focused;
                int handled=0;
                if(f && axmui_str_case_eq(f->tag,"input")){
                    if(ev.code==127||ev.code==8){
                        axmui_input_backspace(f);
                        if(f->on_input) f->on_input(f,f->on_input_data); handled=1;
                    } else if(ev.code=='\n'||ev.code=='\r'){
                        if(f->on_change) f->on_change(f,f->on_change_data); handled=1;
                    } else if(ev.code>=32 && ev.code<127){
                        axmui_input_append(f,(char)ev.code);
                        if(f->on_input) f->on_input(f,f->on_input_data); handled=1;
                    }
                    win->needs_render=1;
                } else {
                    if(ev.code=='\n'||ev.code=='\r'){
                        if(f && axmui_str_case_eq(f->tag,"button") && f->on_click){
                            f->on_click(f, f->on_click_data);
                            win->needs_render=1; handled=1;
                        }
                    }
                }
                if(!handled && win->on_key){
                    win->on_key(0, win->on_key_data);
                    /* let handler decide to close */
                }
            }
        }
        /* re-render if dirty */
        axclient_begin(&cx);
        axmui_window_render(win);
        axclient_commit(&cx);
    }
    return 0;
}

/* Convenience: single-window app helper that takes html/css and callbacks.
 * Most axiomeOS desk clients are exactly this shape, so they collapse to:
 *   axmui_app_t *app=axmui_app_create();
 *   axmui_window_t *win=axmui_window_create(app,"Title",0,0);
 *   axmui_window_set_html(win, html, css);
 *   // wire onclick by id
 *   axmui_view_t *btn=axmui_view_find(axmui_window_root(win),"ok");
 *   axmui_view_on_click(btn, on_ok, NULL);
 *   return axmui_app_run(app,argc,argv,"prog");
 */

#endif /* AXMUI_H */
