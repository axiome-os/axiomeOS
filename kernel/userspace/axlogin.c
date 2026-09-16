/* axlogin — GUI login for axiomeOS (/Binaries/axlogin).
   Runs only as a compositor client: `axlogin --wm <shmid> <w> <h> <evfd> ...`.
   New alongside the CLI /bin/login, which is unchanged.

   Shows a username/password form. On success it authenticates against the
   kernel user database, records the session (uid/gid) in
   /var/run/axlogin.<shmid> and exits 0. The desktop (guixd) consumes that
   file on reap and spawns the per-user terminal itself, tracked, in the
   same slot — axlogin never spawns (no untracked grandchildren, no lost
   pids). The desktop opens this client automatically at boot so nobody has
   to type `login` by hand. */

#include "axclient.h"
#include "axform.h"

#define BTN_LOGIN  1
#define BTN_CANCEL 2

#define AXLOGIN_RUN_DIR "/var/run"

/* Scrub a form field's buffer as well as its length (axform_clear_field
   only resets len, which would leave the password in memory). */
static void login_scrub_field(struct axform *f, int idx)
{
    if (!f || idx < 0 || idx >= f->nfields)
        return;
    memset(f->fields[idx].buf, 0, sizeof(f->fields[idx].buf));
    f->fields[idx].len = 0;
}

/* Record "<uid> <gid>\n" for the desktop to consume on our reap. The
   desktop spawns the session itself, so it always knows (and tracks) the
   session process — and every later terminal it opens for this user. */
static int write_session_file(long shmid, uid_t uid, gid_t gid)
{
    char path[64];
    char body[64];
    int fd;
    size_t off = 0;
    size_t len;
    if (shmid <= 0)
        return -1;
    mkdir(AXLOGIN_RUN_DIR); /* ignore EEXIST: ensure the dir is there */
    snprintf(path, sizeof(path), "%s/axlogin.%ld", AXLOGIN_RUN_DIR, shmid);
    snprintf(body, sizeof(body), "%u %u\n", (unsigned)uid, (unsigned)gid);
    len = strlen(body);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    while (off < len)
    {
        long r = write(fd, body + off, len - off);
        if (r <= 0)
        {
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

static int do_login(struct axform *f, struct axclient *cx)
{
    char user[32];
    char pass[64];
    uid_t uid;
    gid_t gid;
    size_t i;
    if (f->fields[0].len == 0 || f->fields[1].len == 0)
    {
        axform_set_status(f, "Enter a username and password.");
        return 0;
    }
    for (i = 0; i < sizeof(user) - 1 && f->fields[0].buf[i]; i++)
        user[i] = f->fields[0].buf[i];
    user[i] = 0;
    for (i = 0; i < sizeof(pass) - 1 && f->fields[1].buf[i]; i++)
        pass[i] = f->fields[1].buf[i];
    pass[i] = 0;

    if (sys_authenticate(user, pass) < 0)
    {
        axclient_clear_secret(pass, sizeof(pass));
        login_scrub_field(f, 1);
        axform_set_status(f, "Login failed.");
        return 0;
    }
    axclient_clear_secret(pass, sizeof(pass));
    login_scrub_field(f, 1);
    if (sys_getpwnam(user, &uid, &gid) < 0)
    {
        axform_set_status(f, "Login failed.");
        return 0;
    }
    axform_set_status(f, "");
    /* Authenticated: hand the session to the desktop and exit. It spawns
       the terminal itself once this window is reaped. */
    if (write_session_file(cx->shmid, uid, gid) < 0)
    {
        axform_set_status(f, "Could not record the session.");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct axclient cx;
    struct axform form;
    int nouser;
    uint32_t prev_btn = 0;
    if (axclient_init(&cx, argc, argv, "axlogin") < 0)
        return 1;

    nouser = !axclient_has_regular_user();
    axform_init(&form);
    axform_add_field(&form, "Username", 31, 0);
    axform_add_field(&form, "Password", 63, 1);
    form.fields[0].x = 24; form.fields[0].y = 64;  form.fields[0].w = 300;
    form.fields[1].x = 24; form.fields[1].y = 118; form.fields[1].w = 300;
    {
        int b = axform_add_button(&form, "Login", BTN_LOGIN, 0);
        form.buttons[b].x = 24;  form.buttons[b].y = 180;
        form.buttons[b].w = 96;  form.buttons[b].h = FONT_HEIGHT + 10;
        b = axform_add_button(&form, "Cancel", BTN_CANCEL, 0);
        form.buttons[b].x = 132; form.buttons[b].y = 180;
        form.buttons[b].w = 96;  form.buttons[b].h = FONT_HEIGHT + 10;
    }
    if (nouser)
        axform_set_status(&form,
            "No user account exists yet: run First-boot setup first.");

    axclient_begin(&cx);
    axform_draw(&cx.fb, &form, "axiomeOS login");
    cx.hdr->ready = 1;
    axclient_commit(&cx);

    for (;;)
    {
        struct wm_event ev;
        int id;
        if (axclient_closed(&cx) || axclient_stale(&cx))
            break;
        if (axgui_wm_recv(cx.evfd, &ev) < 0)
            break;
        if (axclient_closed(&cx) || axclient_stale(&cx))
            break;
        if (ev.type == WM_EV_MOUSE)
        {
            axform_hover(&form, ev.x, ev.y);
            if ((ev.code & AXINPUT_BTN_LEFT) &&
                !(prev_btn & AXINPUT_BTN_LEFT))
            {
                id = axform_click(&form, ev.x, ev.y);
                if (id == BTN_LOGIN)
                {
                    /* do_login records the session and returns 1: exit so
                       the desktop can spawn it in this same window. */
                    if (nouser)
                        axform_set_status(&form,
                            "No user account exists yet: run First-boot setup first.");
                    else if (do_login(&form, &cx))
                        return 0;
                }
                else if (id == BTN_CANCEL)
                {
                    return 0;
                }
            }
            prev_btn = ev.code;
        }
        else if (ev.type == WM_EV_KEY)
        {
            if (axform_key(&form, ev.code))
            {
                if (nouser)
                    axform_set_status(&form,
                        "No user account exists yet: run First-boot setup first.");
                else if (do_login(&form, &cx))
                    return 0;
            }
        }
        axclient_begin(&cx);
        axform_draw(&cx.fb, &form, "axiomeOS login");
        axclient_commit(&cx);
    }
    return 0;
}
