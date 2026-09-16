/* axoobe — GUI first-boot setup for axiomeOS (/Binaries/axoobe).
   Runs only as a compositor client: `axoobe --wm <shmid> <w> <h> <evfd> ...`.
   New alongside the CLI /bin/oobe, which is unchanged.

   Same job as the CLI wizard: create the first username/password account,
   optionally administrator, with a SHA-256 password hash the kernel verifier
   accepts. Exits 0 on success so the desktop can chain into the login
   window; exits 1 on cancel/failure. A no-op (with a message) when a regular
   user already exists.

   The account is appended to /etc/passwd (O_APPEND, never truncated), so a
   short write can at worst leave a partial tail line, never destroy the
   existing users. All WM glue comes from axclient.h. */

#include "axclient.h"
#include "axform.h"
#include "axsha256.h"

#define PASSWD_PATH "/etc/passwd"
#define USERS_DIR   "/Users"

#define BTN_CREATE 1
#define BTN_CANCEL 2
#define BTN_ADMIN  3

static int valid_username(const char *s)
{
    if (s[0] == 0 || strlen(s) > 31)
        return 0;
    while (*s)
    {
        char c = *s++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

/* Scrub a form field's buffer as well as its length (axform_clear_field
   only resets len, which would leave the password in memory). */
static void oobe_scrub_field(struct axform *f, int idx)
{
    if (!f || idx < 0 || idx >= f->nfields)
        return;
    memset(f->fields[idx].buf, 0, sizeof(f->fields[idx].buf));
    f->fields[idx].len = 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    if (!buf)
        return -1;
    while (off < len)
    {
        long r = write(fd, buf + off, len - off);
        if (r <= 0)
            return -1;
        off += (size_t)r;
    }
    return 0;
}

static int create_account(struct axform *f)
{
    char *name = f->fields[0].buf;
    char pass[64];
    char confirm[64];
    int admin = f->buttons[0].on;
    char dir[160];
    char old[AXCLIENT_PASSWD_MAX + 1];
    char entry[256];
    char hash[65];
    size_t old_len = 0;
    size_t i;
    int fd;

    if (!valid_username(name))
    {
        axform_set_status(f, "Bad username: letters, digits, _ - . (max 31).");
        return 0;
    }
    for (i = 0; i < sizeof(pass) - 1 && f->fields[1].buf[i]; i++)
        pass[i] = f->fields[1].buf[i];
    pass[i] = 0;
    for (i = 0; i < sizeof(confirm) - 1 && f->fields[2].buf[i]; i++)
        confirm[i] = f->fields[2].buf[i];
    confirm[i] = 0;
    if (strlen(pass) < 4)
    {
        axclient_clear_secret(pass, sizeof(pass));
        axclient_clear_secret(confirm, sizeof(confirm));
        axform_set_status(f, "Password must be at least 4 characters.");
        return 0;
    }
    if (strcmp(pass, confirm) != 0)
    {
        axclient_clear_secret(pass, sizeof(pass));
        axclient_clear_secret(confirm, sizeof(confirm));
        oobe_scrub_field(f, 2);
        axform_set_status(f, "Passwords do not match.");
        return 0;
    }

    snprintf(dir, sizeof(dir), "%s/%s", USERS_DIR, name);
    if (mkdir(dir) != 0)
    {
        struct stat st;
        if (stat(dir, &st) != 0 || !(st.st_mode & S_IFDIR))
        {
            axclient_clear_secret(pass, sizeof(pass));
            axclient_clear_secret(confirm, sizeof(confirm));
            axform_set_status(f, "Could not create the home directory.");
            return 0;
        }
    }
    chown(dir, 1000, 1000);

    /* Re-read under the race window: another setup run may have won. */
    if (axclient_read_whole(PASSWD_PATH, old, sizeof(old), &old_len) < 0)
    {
        axclient_clear_secret(pass, sizeof(pass));
        axclient_clear_secret(confirm, sizeof(confirm));
        axform_set_status(f, "Could not read /etc/passwd.");
        return 0;
    }
    if (axclient_has_regular_user())
    {
        axclient_clear_secret(pass, sizeof(pass));
        axclient_clear_secret(confirm, sizeof(confirm));
        return 1; /* another run won the race; treat as done */
    }

    axsha256_hex(pass, hash);
    axclient_clear_secret(pass, sizeof(pass));
    axclient_clear_secret(confirm, sizeof(confirm));
    oobe_scrub_field(f, 1);
    oobe_scrub_field(f, 2);

    {
        int n = snprintf(entry, sizeof(entry), "%s:%s:1000:1000:%s:%s/%s:/bin/sh\n",
                         name, hash, admin ? "admin" : "user", USERS_DIR,
                         name);
        if (n <= 0 || (size_t)n >= sizeof(entry))
        {
            axform_set_status(f, "Account entry too long.");
            return 0;
        }
    }

    /* Append-only: existing users are never rewritten, so a failed write
       cannot corrupt them. */
    fd = open(PASSWD_PATH, O_WRONLY | O_APPEND);
    if (fd < 0)
    {
        axform_set_status(f, "Could not update /etc/passwd.");
        return 0;
    }
    if (old_len > 0 && old[old_len - 1] != '\n')
    {
        if (write_all(fd, "\n", 1) < 0)
        {
            close(fd);
            axform_set_status(f, "Could not update /etc/passwd.");
            return 0;
        }
    }
    if (write_all(fd, entry, strlen(entry)) < 0)
    {
        close(fd);
        axform_set_status(f, "Could not update /etc/passwd.");
        return 0;
    }
    close(fd);

    if (sys_reload_users() != 0)
    {
        axform_set_status(f, "Account created; user DB refresh failed.");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct axclient cx;
    struct axform form;
    uint32_t prev_btn = 0;
    if (axclient_init(&cx, argc, argv, "axoobe") < 0)
        return 1;

    if (axclient_has_regular_user())
    {
        axclient_begin(&cx);
        axgui_fill(&cx.fb, 0, 0, (int)cx.fb.w, (int)cx.fb.h, AXFORM_BG);
        axgui_text(&cx.fb, "First-boot setup", 24, 16, AXFORM_HD, AXFORM_BG);
        axgui_text(&cx.fb, "A user account already exists: nothing to set up.",
                   24, 64, AXFORM_FG, AXFORM_BG);
        axgui_text(&cx.fb, "Press any key to close.", 24, 96, AXFORM_DIM,
                   AXFORM_BG);
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
                break;
        }
        return 0;
    }

    axform_init(&form);
    axform_add_field(&form, "Username", 31, 0);
    axform_add_field(&form, "Password", 63, 1);
    axform_add_field(&form, "Confirm password", 63, 1);
    form.fields[0].x = 24; form.fields[0].y = 56;  form.fields[0].w = 300;
    form.fields[1].x = 24; form.fields[1].y = 110; form.fields[1].w = 300;
    form.fields[2].x = 24; form.fields[2].y = 164; form.fields[2].w = 300;
    {
        int b = axform_add_button(&form, "Administrator", BTN_ADMIN, 1);
        form.buttons[b].x = 24;  form.buttons[b].y = 216;
        form.buttons[b].w = 190; form.buttons[b].h = FONT_HEIGHT + 10;
        b = axform_add_button(&form, "Create", BTN_CREATE, 0);
        form.buttons[b].x = 24;  form.buttons[b].y = 258;
        form.buttons[b].w = 110; form.buttons[b].h = FONT_HEIGHT + 10;
        b = axform_add_button(&form, "Cancel", BTN_CANCEL, 0);
        form.buttons[b].x = 146; form.buttons[b].y = 258;
        form.buttons[b].w = 110; form.buttons[b].h = FONT_HEIGHT + 10;
    }

    axclient_begin(&cx);
    axform_draw(&cx.fb, &form, "axiomeOS first-boot setup");
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
        if (ev.type == WM_EV_MOUSE)
        {
            axform_hover(&form, ev.x, ev.y);
            if ((ev.code & AXINPUT_BTN_LEFT) &&
                !(prev_btn & AXINPUT_BTN_LEFT))
            {
                int id = axform_click(&form, ev.x, ev.y);
                if (id == BTN_CREATE)
                {
                    if (create_account(&form))
                    {
                        /* Success: publish the confirmation and exit 0
                           at once so the desktop chains into login even
                           if the window is closed on this exact frame. */
                        axform_set_status(&form, "Account created.");
                        axclient_begin(&cx);
                        axform_draw(&cx.fb, &form,
                                    "axiomeOS first-boot setup");
                        axclient_commit(&cx);
                        return 0;
                    }
                }
                else if (id == BTN_CANCEL)
                {
                    return 1;
                }
            }
            prev_btn = ev.code;
        }
        else if (ev.type == WM_EV_KEY)
        {
            if (axform_key(&form, ev.code))
            {
                if (create_account(&form))
                {
                    axform_set_status(&form, "Account created.");
                    axclient_begin(&cx);
                    axform_draw(&cx.fb, &form, "axiomeOS first-boot setup");
                    axclient_commit(&cx);
                    return 0;
                }
            }
        }
        axclient_begin(&cx);
        axform_draw(&cx.fb, &form, "axiomeOS first-boot setup");
        axclient_commit(&cx);
    }
    return 1;
}
