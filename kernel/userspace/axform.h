/* axform.h — tiny immediate-mode form UI for GUI window clients.
   Shared by axlogin (login form) and axoobe (first-boot setup form).
   Header-only, like axgui.h. Clients own layout coordinates; this header
   owns field editing, focus, buttons, toggles and drawing.

   Input model: keyboard Tab/Up/Down cycles fields, Enter submits, printable
   characters edit the focused field; mouse press focuses fields and activates
   buttons (toggle buttons flip instead). The client translates compositor
   events into axform_key()/axform_click() calls and redraws after each one. */

#ifndef AXFORM_H
#define AXFORM_H

#include "axgui.h"

#define AXFORM_MAX_FIELDS 4
#define AXFORM_MAX_BUTTONS 3
#define AXFORM_FIELD_CAP 64
#define AXFORM_LABEL_CAP 28
#define AXFORM_STATUS_CAP 128

#define AXFORM_BG     0x101418u
#define AXFORM_FG     0xD8DEE9u
#define AXFORM_HD     0x88C0D0u
#define AXFORM_DIM    0x4C566Au
#define AXFORM_BOX    0x0B0F14u
#define AXFORM_ACCENT 0x5E81ACu
#define AXFORM_BTN    0x2B3B55u
#define AXFORM_BTNHOT 0x3D5A80u
#define AXFORM_ERR    0xBF616Au

struct axform_field {
    char label[AXFORM_LABEL_CAP];
    char buf[AXFORM_FIELD_CAP];
    int len;
    int cap;      /* usable chars, < AXFORM_FIELD_CAP */
    int masked;   /* render asterisks (passwords) */
    int x, y, w;  /* layout: label at (x,y), box below it */
};

struct axform_button {
    char label[AXFORM_LABEL_CAP];
    int x, y, w, h;
    int id;       /* returned by axform_click when activated */
    int toggle;   /* checkbox behaviour: click flips `on` */
    int on;
    int hot;      /* pointer currently inside */
};

struct axform {
    struct axform_field fields[AXFORM_MAX_FIELDS];
    int nfields;
    int focus;    /* focused field index */
    struct axform_button buttons[AXFORM_MAX_BUTTONS];
    int nbuttons;
    char status[AXFORM_STATUS_CAP];
};

static void axform_init(struct axform *f)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < AXFORM_MAX_FIELDS; i++)
    {
        f->fields[i].label[0] = 0;
        f->fields[i].buf[0] = 0;
        f->fields[i].len = 0;
        f->fields[i].cap = 0;
        f->fields[i].masked = 0;
        f->fields[i].x = f->fields[i].y = f->fields[i].w = 0;
    }
    for (i = 0; i < AXFORM_MAX_BUTTONS; i++)
    {
        f->buttons[i].label[0] = 0;
        f->buttons[i].x = f->buttons[i].y = 0;
        f->buttons[i].w = f->buttons[i].h = 0;
        f->buttons[i].id = 0;
        f->buttons[i].toggle = 0;
        f->buttons[i].on = 0;
        f->buttons[i].hot = 0;
    }
    f->nfields = 0;
    f->focus = 0;
    f->nbuttons = 0;
    f->status[0] = 0;
}

/* Returns the field index, or -1 when full. */
static int axform_add_field(struct axform *f, const char *label, int cap,
                            int masked)
{
    struct axform_field *d;
    size_t i;
    if (!f || !label || f->nfields >= AXFORM_MAX_FIELDS)
        return -1;
    if (cap < 1)
        cap = 1;
    if (cap > AXFORM_FIELD_CAP - 1)
        cap = AXFORM_FIELD_CAP - 1;
    d = &f->fields[f->nfields];
    for (i = 0; i + 1 < sizeof(d->label) && label[i]; i++)
        d->label[i] = label[i];
    d->label[i] = 0;
    d->buf[0] = 0;
    d->len = 0;
    d->cap = cap;
    d->masked = masked;
    return f->nfields++;
}

/* Returns the button index, or -1 when full. */
static int axform_add_button(struct axform *f, const char *label, int id,
                             int toggle)
{
    struct axform_button *b;
    size_t i;
    if (!f || !label || f->nbuttons >= AXFORM_MAX_BUTTONS)
        return -1;
    b = &f->buttons[f->nbuttons];
    for (i = 0; i + 1 < sizeof(b->label) && label[i]; i++)
        b->label[i] = label[i];
    b->label[i] = 0;
    b->id = id;
    b->toggle = toggle;
    b->on = 0;
    b->hot = 0;
    return f->nbuttons++;
}

static void axform_set_status(struct axform *f, const char *s)
{
    size_t i;
    if (!f)
        return;
    if (!s)
    {
        f->status[0] = 0;
        return;
    }
    for (i = 0; i + 1 < sizeof(f->status) && s[i]; i++)
        f->status[i] = s[i];
    f->status[i] = 0;
}

static void axform_clear_field(struct axform *f, int idx)
{
    if (!f || idx < 0 || idx >= f->nfields)
        return;
    f->fields[idx].buf[0] = 0;
    f->fields[idx].len = 0;
}

/* Keyboard input. Returns 1 when Enter requests submit, 0 otherwise. */
static int axform_key(struct axform *f, uint32_t code)
{
    struct axform_field *d;
    if (!f || f->nfields == 0)
        return 0;
    if (f->focus < 0 || f->focus >= f->nfields)
        f->focus = 0;
    if (code == '\n' || code == '\r')
        return 1;
    if (code == '\t' || code == AXINPUT_KEY_DOWN)
    {
        f->focus = (f->focus + 1) % f->nfields;
        return 0;
    }
    if (code == AXINPUT_KEY_UP)
    {
        f->focus = (f->focus + f->nfields - 1) % f->nfields;
        return 0;
    }
    d = &f->fields[f->focus];
    if (code == 127 || code == 8)
    {
        if (d->len > 0)
        {
            d->len--;
            d->buf[d->len] = 0;
        }
        return 0;
    }
    if (code >= 32 && code < 127 && d->len < d->cap)
    {
        d->buf[d->len++] = (char)code;
        d->buf[d->len] = 0;
    }
    return 0;
}

static int axform_inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* Mouse press at content-local (x, y). Focuses fields, flips toggles and
   returns the activated momentary button id, or 0 when nothing fired. */
static int axform_click(struct axform *f, int x, int y)
{
    int i;
    if (!f)
        return 0;
    for (i = 0; i < f->nfields; i++)
    {
        struct axform_field *d = &f->fields[i];
        int by = d->y + FONT_HEIGHT + 2;
        int bh = FONT_HEIGHT + 8;
        if (axform_inside(x, y, d->x, by, d->w, bh))
        {
            f->focus = i;
            return 0;
        }
    }
    for (i = 0; i < f->nbuttons; i++)
    {
        struct axform_button *b = &f->buttons[i];
        if (axform_inside(x, y, b->x, b->y, b->w, b->h))
        {
            if (b->toggle)
            {
                b->on = !b->on;
                return 0;
            }
            return b->id;
        }
    }
    return 0;
}

/* Pointer hover highlight for buttons (call on mouse motion). */
static void axform_hover(struct axform *f, int x, int y)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < f->nbuttons; i++)
    {
        struct axform_button *b = &f->buttons[i];
        b->hot = axform_inside(x, y, b->x, b->y, b->w, b->h);
    }
}

static void axform_draw(struct axgui_fb *fb, struct axform *f,
                        const char *title)
{
    int i;
    char dots[AXFORM_FIELD_CAP];
    if (!fb || !fb->px || !f)
        return;
    axgui_fill(fb, 0, 0, (int)fb->w, (int)fb->h, AXFORM_BG);
    if (title)
        axgui_text(fb, title, 24, 16, AXFORM_HD, AXFORM_BG);
    for (i = 0; i < f->nfields; i++)
    {
        struct axform_field *d = &f->fields[i];
        int by = d->y + FONT_HEIGHT + 2;
        int bh = FONT_HEIGHT + 8;
        int focused = (i == f->focus);
        int tx = d->x + 6;
        int ty = by + 4;
        const char *shown = d->buf;
        axgui_text(fb, d->label, d->x, d->y, AXFORM_FG, AXFORM_BG);
        axgui_fill(fb, d->x, by, d->w, bh, AXFORM_BOX);
        axgui_rect(fb, d->x, by, d->w, bh,
                   focused ? AXFORM_ACCENT : AXFORM_DIM);
        if (d->masked)
        {
            int k;
            for (k = 0; k < d->len && k < AXFORM_FIELD_CAP - 1; k++)
                dots[k] = '*';
            dots[k] = 0;
            shown = dots;
        }
        axgui_text(fb, shown, tx, ty, AXFORM_FG, AXFORM_BOX);
        if (focused)
        {
            int cx = tx + d->len * FONT_WIDTH;
            axgui_fill(fb, cx, ty, FONT_WIDTH, FONT_HEIGHT, AXFORM_ACCENT);
        }
    }
    for (i = 0; i < f->nbuttons; i++)
    {
        struct axform_button *b = &f->buttons[i];
        char blabel[AXFORM_LABEL_CAP + 5];
        size_t p = 0;
        uint32_t bg = b->hot ? AXFORM_BTNHOT : AXFORM_BTN;
        if (b->toggle)
        {
            blabel[p++] = '[';
            blabel[p++] = b->on ? 'x' : ' ';
            blabel[p++] = ']';
            blabel[p++] = ' ';
        }
        {
            size_t k;
            for (k = 0; b->label[k] && p + 1 < sizeof(blabel); k++)
                blabel[p++] = b->label[k];
        }
        blabel[p] = 0;
        axgui_fill(fb, b->x, b->y, b->w, b->h, bg);
        axgui_rect(fb, b->x, b->y, b->w, b->h, AXFORM_ACCENT);
        axgui_text(fb, blabel, b->x + 8, b->y + 5, AXFORM_FG, bg);
    }
    if (f->status[0])
        axgui_text(fb, f->status, 24, (int)fb->h - FONT_HEIGHT - 12,
                   AXFORM_ERR, AXFORM_BG);
}

#endif
