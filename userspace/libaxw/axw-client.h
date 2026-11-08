#ifndef AXIOME_AXW_CLIENT_H
#define AXIOME_AXW_CLIENT_H

#include "axw-protocol.h"
#include <stddef.h>
#include <stdint.h>

/* ---- opaque proxy types ---- */

struct axw_display;
struct axw_registry;
struct axw_compositor;
struct axw_shm;
struct axw_seat;
struct axw_output;
struct axw_surface;
struct axw_callback;

/* ---- buffer (full definition exposed so clients can read buffer->id) ---- */

struct axw_buffer {
    uint32_t id;
    long shmid;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

/* ---- connection ---- */

/* Open a connection to the display server. Returns fd on success, -1 on fail. */
int axw_connect(const char *path);

/* ---- display object ---- */

struct axw_display *axw_display_create(int fd);
void axw_display_destroy(struct axw_display *d);

/* Round-trip: send sync request, wait for callback done. */
int axw_display_sync(struct axw_display *d);

/* Terminate the connection. */
int axw_display_terminate(struct axw_display *d);

/* Get the registry global. */
struct axw_registry *axw_display_get_registry(struct axw_display *d);

/* ---- registry ---- */

struct axw_registry *axw_registry_create(int fd, struct axw_display *d);
void axw_registry_destroy(struct axw_registry *r);

/* Bind to a global object. */
struct axw_compositor *axw_registry_bind_compositor(struct axw_registry *r);
struct axw_shm *axw_registry_bind_shm(struct axw_registry *r);
struct axw_seat *axw_registry_bind_seat(struct axw_registry *r);
struct axw_output *axw_registry_bind_output(struct axw_registry *r);

/* ---- compositor ---- */

struct axw_compositor *axw_compositor_create(int fd, struct axw_display *d);
void axw_compositor_destroy(struct axw_compositor *c);

struct axw_surface *axw_compositor_create_surface(struct axw_compositor *c);

/* ---- shm ---- */

struct axw_shm *axw_shm_create(int fd, struct axw_display *d);
void axw_shm_destroy(struct axw_shm *s);

/* Create a shared memory pool backed by a shm segment (GEM object).
   Returns a proxy; the pool object itself is server-side. */
struct axw_buffer *axw_shm_create_pool(struct axw_shm *s, long shmid,
                                       uint32_t width, uint32_t height,
                                       uint32_t format);

/* ---- buffer ---- */

void axw_buffer_destroy(struct axw_buffer *b);

/* ---- surface ---- */

struct axw_surface *axw_surface_create(int fd, struct axw_display *d);
void axw_surface_destroy(struct axw_surface *s);

int axw_surface_attach(struct axw_surface *s, struct axw_buffer *b,
                       int32_t x, int32_t y);
int axw_surface_damage(struct axw_surface *s,
                       int32_t x, int32_t y, int32_t w, int32_t h);
int axw_surface_commit(struct axw_surface *s);

/* Get the surface's wire object ID (for event filtering). */
uint32_t axw_surface_id(struct axw_surface *s);

/* Get the buffer's wire object ID. */
uint32_t axw_buffer_id(struct axw_buffer *b);

/* ---- seat ---- */

struct axw_seat *axw_seat_create(int fd, struct axw_display *d);
void axw_seat_destroy(struct axw_seat *seat);

struct axw_pointer *axw_seat_get_pointer(struct axw_seat *seat);
struct axw_keyboard *axw_seat_get_keyboard(struct axw_seat *seat);

/* ---- pointer / keyboard (opaque, created via seat) ---- */

struct axw_pointer;
struct axw_keyboard;

void axw_pointer_destroy(struct axw_pointer *p);
void axw_keyboard_destroy(struct axw_keyboard *k);

/* ---- event dispatch ---- */

/* Dispatch pending events from the connection. Calls the appropriate
   handler callbacks set via axw_display_set_*_handler. Returns number of
   events processed, or -1 on error. */
int axw_dispatch(struct axw_display *d);

/* ---- callback ---- */

struct axw_callback *axw_display_frame(struct axw_display *d);
void axw_callback_destroy(struct axw_callback *cb);

#endif
