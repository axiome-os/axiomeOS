#ifndef AXIOME_WINSYS_H
#define AXIOME_WINSYS_H

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "axdri.h"

#define AXIOME_MAX_DUMB 16

struct axiome_winsys_handle {
    struct axdri_buffer buf;
    void *cpu_ptr; /* mmap'd pointer from axdri_map */
};

struct axiome_winsys_screen {
    struct pipe_screen base;
    int fd;
    uint32_t width, height, pitch;
    unsigned nr_dumb;
    struct axiome_winsys_handle dumb[AXIOME_MAX_DUMB];
};

struct axiome_winsys_context {
    struct pipe_context base;
    struct pipe_screen *screen;
    void *softpipe_ctx; /* softpipe pipe_context, wrapped */
};

struct axiome_winsys_resource {
    struct pipe_resource base;
    struct axiome_winsys_handle *dumb; /* NULL for non-display resources */
};

struct axiome_winsys_surface {
    struct pipe_surface base;
};

struct pipe_screen *axiome_create_screen(int fd);
struct pipe_context *axiome_create_context(struct pipe_screen *screen, void *priv, unsigned flags);

#endif /* AXIOME_WINSYS_H */
