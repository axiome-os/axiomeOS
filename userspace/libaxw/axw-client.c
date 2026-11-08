/* axw-client.c — client library for the axiome display server protocol.
   Marshals requests to /devices/axw-0, parses incoming events, manages
   proxy objects. Linked into every GUI client (axterm, axinfo, ...). */

#include "axw-client.h"
#include "axgui.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- connection state ---- */

struct axw_connection {
    int fd;
    uint32_t next_id;       /* next client-created object id */
    int (*event_handler)(struct axw_display *d, uint32_t sender_id,
                         uint16_t opcode, const uint32_t *args, int nargs);
    void *handler_data;
};

/* ---- proxy objects ---- */

struct axw_display {
    struct axw_connection *conn;
    struct axw_registry *registry;
    struct axw_compositor *compositor;
    struct axw_shm *shm;
    struct axw_seat *seat;
    struct axw_output *output;
};

struct axw_registry {
    struct axw_connection *conn;
    struct axw_display *display;
};

struct axw_compositor {
    struct axw_connection *conn;
    struct axw_display *display;
};

struct axw_shm {
    struct axw_connection *conn;
    struct axw_display *display;
};

struct axw_seat {
    struct axw_connection *conn;
    struct axw_display *display;
    struct axw_pointer *pointer;
    struct axw_keyboard *keyboard;
};

struct axw_output {
    struct axw_connection *conn;
    struct axw_display *display;
};

struct axw_surface {
    uint32_t id;
    struct axw_connection *conn;
    struct axw_buffer *attached;
    int32_t attach_x, attach_y;
    struct axw_damage damages[8];
    int ndamage;
};

struct axw_pointer {
    struct axw_seat *seat;
};

struct axw_keyboard {
    struct axw_seat *seat;
};

struct axw_callback {
    uint32_t id;
    struct axw_connection *conn;
};

/* ---- global connection (single per process) ---- */

static struct axw_connection *g_conn = 0;

/* ---- message helpers ---- */

static int axw_send_request(int fd, uint32_t sender_id, uint16_t opcode,
                            const uint32_t *args, int nargs)
{
    if (nargs < 0 || nargs > AXW_MAX_ARGS)
        return -1;
    uint16_t size = (uint16_t)(8u + (uint32_t)nargs * 4u);
    uint8_t buf[8 + AXW_MAX_ARGS * 4];
    struct axw_msg_hdr *hdr = (struct axw_msg_hdr *)buf;
    hdr->sender_id = sender_id;
    hdr->opcode = opcode;
    hdr->size = size;
    if (nargs > 0)
        __builtin_memcpy(buf + 8, args, (size_t)nargs * 4u);
    if (write(fd, buf, size) != (long)size)
        return -1;
    return 0;
}

static int axw_read_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;
    uint8_t *p = (uint8_t *)buf;
    while (got < len)
    {
        long r = read(fd, p + got, len - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int axw_read_message(int fd, struct axw_msg_hdr *hdr,
                            uint32_t *args)
{
    uint8_t buf[8 + AXW_MAX_ARGS * 4];
    /* Read the full message in one go. axw_dev_read_conn copies the entire
       message (header + args) into the buffer, consuming one queue entry. */
    long r = read(fd, buf, sizeof(buf));
    if (r <= 0)
        return -1;
    if (r < (long)sizeof(*hdr))
        return -1;
    __builtin_memcpy(hdr, buf, sizeof(*hdr));
    uint16_t total = hdr->size;
    if (r < (long)total)
        return -1;
    int nargs = (int)((total - 8u) / 4u);
    if (nargs > 0)
        __builtin_memcpy(args, buf + 8, (size_t)nargs * 4u);
    return nargs;
}

/* ---- forward declarations (functions used before definition) ---- */

struct axw_output *axw_output_create(int fd, struct axw_display *d);
struct axw_seat *axw_seat_create(int fd, struct axw_display *d);

/* ---- connection ---- */

int axw_connect(const char *path)
{
    int fd = open(path, 2 /* O_RDWR */);
    if (fd < 0)
        return -1;
    fcntl(fd, 4 /* F_SETFL */, 04000 /* O_NONBLOCK */);
    return fd;
}

static struct axw_connection *axw_connection_create(int fd)
{
    struct axw_connection *c = (struct axw_connection *)
        malloc(sizeof(*c));
    if (!c)
        return 0;
    c->fd = fd;
    c->next_id = 1;
    c->event_handler = 0;
    c->handler_data = 0;
    return c;
}

/* ---- display ---- */

struct axw_display *axw_display_create(int fd)
{
    struct axw_connection *c = axw_connection_create(fd);
    if (!c)
        return 0;
    g_conn = c;

    struct axw_display *d = (struct axw_display *)
        malloc(sizeof(*d));
    if (!d)
    {
        free(c);
        return 0;
    }
    memset(d, 0, sizeof(*d));
    d->conn = c;
    return d;
}

void axw_display_destroy(struct axw_display *d)
{
    if (!d)
        return;
    if (d->conn)
        close(d->conn->fd);
    free(d->conn);
    free(d);
    g_conn = 0;
}

int axw_display_sync(struct axw_display *d)
{
    (void)d;
    return 0;
}

int axw_display_terminate(struct axw_display *d)
{
    if (!d || !d->conn)
        return -1;
    uint32_t args[1] = { d->conn->next_id++ };
    return axw_send_request(d->conn->fd, AXW_ID_DISPLAY,
                            AXW_DISPLAY_TERMINATE, args, 1);
}

struct axw_registry *axw_display_get_registry(struct axw_display *d)
{
    if (!d || !d->conn)
        return 0;
    if (!d->registry)
    {
        d->registry = axw_registry_create(d->conn->fd, d);
    }
    return d->registry;
}

/* ---- registry ---- */

struct axw_registry *axw_registry_create(int fd, struct axw_display *d)
{
    struct axw_registry *r = (struct axw_registry *)
        malloc(sizeof(*r));
    if (!r)
        return 0;
    memset(r, 0, sizeof(*r));
    r->conn = d->conn;
    r->display = d;
    uint32_t args[2] = { AXW_ID_REGISTRY, d->conn->next_id++ };
    axw_send_request(fd, AXW_ID_DISPLAY, AXW_DISPLAY_GET_REGISTRY, args, 2);
    return r;
}

void axw_registry_destroy(struct axw_registry *r)
{
    if (!r)
        return;
    free(r);
}

struct axw_compositor *axw_registry_bind_compositor(struct axw_registry *r)
{
    if (!r || !r->conn || !r->display)
        return 0;
    if (!r->display->compositor)
    {
        r->display->compositor = axw_compositor_create(r->conn->fd, r->display);
    }
    return r->display->compositor;
}

struct axw_shm *axw_registry_bind_shm(struct axw_registry *r)
{
    if (!r || !r->conn || !r->display)
        return 0;
    if (!r->display->shm)
    {
        r->display->shm = axw_shm_create(r->conn->fd, r->display);
    }
    return r->display->shm;
}

struct axw_seat *axw_registry_bind_seat(struct axw_registry *r)
{
    if (!r || !r->conn || !r->display)
        return 0;
    if (!r->display->seat)
    {
        r->display->seat = axw_seat_create(r->conn->fd, r->display);
    }
    return r->display->seat;
}

struct axw_output *axw_registry_bind_output(struct axw_registry *r)
{
    if (!r || !r->conn || !r->display)
        return 0;
    if (!r->display->output)
    {
        r->display->output = axw_output_create(r->conn->fd, r->display);
    }
    return r->display->output;
}

/* ---- compositor ---- */

struct axw_compositor *axw_compositor_create(int fd, struct axw_display *d)
{
    struct axw_compositor *c = (struct axw_compositor *)
        malloc(sizeof(*c));
    if (!c)
        return 0;
    memset(c, 0, sizeof(*c));
    c->conn = d->conn;
    c->display = d;
    uint32_t args[2] = { AXW_ID_COMPOSITOR, c->conn->next_id++ };
    axw_send_request(fd, AXW_ID_DISPLAY, AXW_DISPLAY_GET_REGISTRY, args, 2);
    return c;
}

void axw_compositor_destroy(struct axw_compositor *c)
{
    if (!c)
        return;
    free(c);
}

struct axw_surface *axw_compositor_create_surface(struct axw_compositor *c)
{
    if (!c || !c->conn)
        return 0;
    return axw_surface_create(c->conn->fd, c->display);
}

/* ---- shm ---- */

struct axw_shm *axw_shm_create(int fd, struct axw_display *d)
{
    struct axw_shm *s = (struct axw_shm *)malloc(sizeof(*s));
    if (!s)
        return 0;
    memset(s, 0, sizeof(*s));
    s->conn = d->conn;
    s->display = d;
    uint32_t args[2] = { AXW_ID_SHM, s->conn->next_id++ };
    axw_send_request(fd, AXW_ID_DISPLAY, AXW_DISPLAY_GET_REGISTRY, args, 2);
    return s;
}

void axw_shm_destroy(struct axw_shm *s)
{
    if (!s)
        return;
    free(s);
}

struct axw_buffer *axw_shm_create_pool(struct axw_shm *s, long shmid,
                                       uint32_t width, uint32_t height,
                                       uint32_t format)
{
    struct axw_buffer *b = (struct axw_buffer *)malloc(sizeof(*b));
    if (!b)
        return 0;
    b->id = s->conn->next_id++;
    b->shmid = shmid;
    b->width = width;
    b->height = height;
    b->format = format;

    uint32_t args[5] = { b->id, (uint32_t)shmid, width, height, format };
    axw_send_request(s->conn->fd, AXW_ID_SHM, AXW_SHM_CREATE_POOL, args, 5);
    return b;
}

/* ---- buffer ---- */

void axw_buffer_destroy(struct axw_buffer *b)
{
    if (!b)
        return;
    free(b);
}

/* ---- surface ---- */

struct axw_surface *axw_surface_create(int fd, struct axw_display *d)
{
    struct axw_surface *s = (struct axw_surface *)
        malloc(sizeof(*s));
    if (!s)
        return 0;
    s->id = d->conn->next_id++;
    s->conn = d->conn;
    s->attached = 0;
    s->attach_x = 0;
    s->attach_y = 0;
    s->ndamage = 0;
    memset(s->damages, 0, sizeof(s->damages));

    uint32_t args[1] = { s->id };
    axw_send_request(fd, AXW_ID_COMPOSITOR, AXW_COMPOSITOR_CREATE_SURFACE,
                     args, 1);
    return s;
}

void axw_surface_destroy(struct axw_surface *s)
{
    if (!s)
        return;
    uint32_t args[1] = { s->id };
    axw_send_request(s->conn->fd, s->id, AXW_SURFACE_DESTROY, args, 1);
    free(s);
}

int axw_surface_attach(struct axw_surface *s, struct axw_buffer *b,
                       int32_t x, int32_t y)
{
    if (!s)
        return -1;
    s->attached = b;
    s->attach_x = x;
    s->attach_y = y;
    uint32_t args[3] = { s->id, b ? b->id : 0u,
                         (uint32_t)((uint32_t)x & 0xFFFFu) |
                         (uint32_t)(((uint32_t)y & 0xFFFFu) << 16) };
    return axw_send_request(s->conn->fd, s->id, AXW_SURFACE_ATTACH, args, 3);
}

int axw_surface_damage(struct axw_surface *s,
                       int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s)
        return -1;
    if (s->ndamage < 8)
    {
        s->damages[s->ndamage].x = x;
        s->damages[s->ndamage].y = y;
        s->damages[s->ndamage].w = w;
        s->damages[s->ndamage].h = h;
        s->ndamage++;
    }
    uint32_t args[3] = { s->id,
                         (uint32_t)((uint32_t)x & 0xFFFFu) |
                         (uint32_t)(((uint32_t)y & 0xFFFFu) << 16),
                         (uint32_t)((uint32_t)w & 0xFFFFu) |
                         (uint32_t)(((uint32_t)h & 0xFFFFu) << 16) };
    return axw_send_request(s->conn->fd, s->id, AXW_SURFACE_DAMAGE, args, 3);
}

int axw_surface_commit(struct axw_surface *s)
{
    if (!s)
        return -1;
    uint32_t args[1] = { s->id };
    int r = axw_send_request(s->conn->fd, s->id, AXW_SURFACE_COMMIT, args, 1);
    s->ndamage = 0;
    return r;
}

uint32_t axw_surface_id(struct axw_surface *s)
{
    return s ? s->id : 0u;
}

uint32_t axw_buffer_id(struct axw_buffer *b)
{
    return b ? b->id : 0u;
}

/* ---- seat ---- */

struct axw_seat *axw_seat_create(int fd, struct axw_display *d)
{
    struct axw_seat *seat = (struct axw_seat *)malloc(sizeof(*seat));
    if (!seat)
        return 0;
    memset(seat, 0, sizeof(*seat));
    seat->conn = d->conn;
    seat->display = d;
    uint32_t args[2] = { AXW_ID_SEAT, seat->conn->next_id++ };
    axw_send_request(fd, AXW_ID_DISPLAY, AXW_DISPLAY_GET_REGISTRY, args, 2);
    return seat;
}

void axw_seat_destroy(struct axw_seat *seat)
{
    if (!seat)
        return;
    free(seat);
}

struct axw_pointer *axw_seat_get_pointer(struct axw_seat *seat)
{
    if (!seat)
        return 0;
    if (!seat->pointer)
    {
        seat->pointer = (struct axw_pointer *)malloc(sizeof(*seat->pointer));
        if (seat->pointer)
            seat->pointer->seat = seat;
    }
    return seat->pointer;
}

struct axw_keyboard *axw_seat_get_keyboard(struct axw_seat *seat)
{
    if (!seat)
        return 0;
    if (!seat->keyboard)
    {
        seat->keyboard = (struct axw_keyboard *)malloc(sizeof(*seat->keyboard));
        if (seat->keyboard)
            seat->keyboard->seat = seat;
    }
    return seat->keyboard;
}

void axw_pointer_destroy(struct axw_pointer *p)
{
    if (!p)
        return;
    if (p->seat)
        p->seat->pointer = 0;
    free(p);
}

void axw_keyboard_destroy(struct axw_keyboard *k)
{
    if (!k)
        return;
    if (k->seat)
        k->seat->keyboard = 0;
    free(k);
}

/* ---- output ---- */

struct axw_output *axw_output_create(int fd, struct axw_display *d)
{
    struct axw_output *o = (struct axw_output *)malloc(sizeof(*o));
    if (!o)
        return 0;
    memset(o, 0, sizeof(*o));
    o->conn = d->conn;
    o->display = d;
    uint32_t args[2] = { AXW_ID_OUTPUT, o->conn->next_id++ };
    axw_send_request(fd, AXW_ID_DISPLAY, AXW_DISPLAY_GET_REGISTRY, args, 2);
    return o;
}

/* ---- callback ---- */

struct axw_callback *axw_display_frame(struct axw_display *d)
{
    if (!d || !d->conn)
        return 0;
    struct axw_callback *cb = (struct axw_callback *)malloc(sizeof(*cb));
    if (!cb)
        return 0;
    cb->id = d->conn->next_id++;
    cb->conn = d->conn;
    return cb;
}

void axw_callback_destroy(struct axw_callback *cb)
{
    if (!cb)
        return;
    free(cb);
}

/* ---- event dispatch ---- */

int axw_dispatch(struct axw_display *d)
{
    (void)d;
    if (!g_conn)
        return 0;
    int fd = g_conn->fd;
    int count = 0;
    for (;;)
    {
        struct axw_msg_hdr hdr;
        uint32_t args[AXW_MAX_ARGS];
        int nargs = axw_read_message(fd, &hdr, args);
        if (nargs < 0)
            break;
        count++;
        if (g_conn->event_handler)
        {
            g_conn->event_handler(d, hdr.sender_id, hdr.opcode, args, nargs);
        }
    }
    return count;
}

void axw_display_set_event_handler(struct axw_display *d,
                                   int (*handler)(struct axw_display *,
                                                  uint32_t, uint16_t,
                                                  const uint32_t *, int),
                                   void *data)
{
    if (!d || !d->conn)
        return;
    d->conn->event_handler = handler;
    d->conn->handler_data = data;
}
