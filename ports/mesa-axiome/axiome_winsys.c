#include "axiome_winsys.h"

#include "util/u_memory.h"
#include "util/u_format.h"
#include "util/u_transfer.h"
#include "util/u_double_list.h"
#include "util/u_viewport.h"
#include "util/u_rect.h"
#include "util/u_prim.h"

struct axiome_winsys_screen *axiome_screen(struct pipe_screen *ps)
{
    return (struct axiome_winsys_screen *)ps;
}

struct axiome_winsys_context *axiome_context(struct pipe_context *pc)
{
    return (struct axiome_winsys_context *)pc;
}

struct axiome_winsys_resource *axiome_resource(struct pipe_resource *pres)
{
    return (struct axiome_winsys_resource *)pres;
}

static void axiome_destroy_screen(struct pipe_screen *pscreen)
{
    struct axiome_winsys_screen *ws = axiome_screen(pscreen);
    unsigned i;
    for (i = 0; i < ws->nr_dumb; i++) {
        if (ws->dumb[i].buf.handle)
            axdri_destroy(ws->fd, &ws->dumb[i].buf);
    }
    if (ws->fd >= 0)
        close(ws->fd);
    FREE(ws);
}

static const char *axiome_get_name(struct pipe_screen *pscreen)
{
    return "axiome-dri0";
}

static int axiome_get_param(struct pipe_screen *pscreen, enum pipe_cap param)
{
    switch (param) {
        case PIPE_CAP_NPOT_TEXTURES:
        case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
        case PIPE_CAP_TWO_SIDED:
        case PIPE_CAP_MAX_TEXTURE_2D_SIZE:
        case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
            return 1;
        case PIPE_CAP_TEXTURE_MIRROR_REPEAT:
        case PIPE_CAP_TEXTURE_SWIZZLE:
        case PIPE_CAP_FRAGMENT_SHADER:
        case PIPE_CAP_VERTEX_SHADER:
            return 1;
        default:
            return 0;
    }
}

static float axiome_get_paramf(struct pipe_screen *pscreen,
                               enum pipe_capf param)
{
    return 0.0f;
}

static int axiome_get_shader_param(struct pipe_screen *pscreen,
                                   enum shader_type sh_type,
                                   enum pipe_shader_cap param)
{
    return 0;
}

static int axiome_is_format_supported(struct pipe_screen *pscreen,
                                      enum pipe_format format,
                                      enum pipe_texture_target target,
                                      unsigned sample_count,
                                      unsigned storage_sample_count,
                                      unsigned bind)
{
    return format == PIPE_FORMAT_B8G8R8X8_UNORM ||
           format == PIPE_FORMAT_X8B8G8R8_UNORM;
}

static void axiome_destroy_context(struct pipe_context *pcontext)
{
    struct axiome_winsys_context *ctx = axiome_context(pcontext);
    FREE(ctx);
}

static void axiome_flush(struct pipe_context *pcontext,
                         struct pipe_fence_handle **fence,
                         unsigned flags)
{
    /* No-op: present is done per-resource via axdri_present at draw time. */
}

static void axiome_clear(struct pipe_context *pcontext,
                         unsigned buffers, const union pipe_color_union *color,
                         double depth, unsigned stencil)
{
    /* softpipe handles clear internally. */
}

static void axiome_draw_vbo(struct pipe_context *pcontext,
                            const struct pipe_draw_info *info)
{
    /* softpipe handles draw internally. */
}

static struct pipe_surface *axiome_create_surface(struct pipe_context *pcontext,
                                                  struct pipe_resource *pres,
                                                  const struct pipe_surface *tmpl)
{
    struct axiome_winsys_surface *surf = CALLOC_STRUCT(axiome_winsys_surface);
    if (!surf)
        return NULL;
    surf->base = *tmpl;
    surf->base.texture = NULL; /* no texture wrapping yet */
    pipe_reference_init(&surf->base.reference, 1);
    surf->base.context = pcontext;
    return &surf->base;
}

static void axiome_surface_destroy(struct pipe_context *pcontext,
                                   struct pipe_surface *psurf)
{
    FREE(psurf);
}

static void axiome_transfer_flush(struct pipe_context *pcontext,
                                  struct pipe_transfer *ptransfer)
{
    /* softpipe tracks dirty regions; no-op here. */
}

static void *axiome_transfer_map(struct pipe_context *pcontext,
                                 struct pipe_resource *pres,
                                 unsigned level, unsigned usage,
                                 const struct pipe_box *box)
{
    /* softpipe handles its own staging buffers; no-op. */
    return NULL;
}

static void axiome_transfer_unmap(struct pipe_context *pcontext,
                                  struct pipe_transfer *ptransfer)
{
}

static struct pipe_resource *axiome_resource_create(struct pipe_screen *pscreen,
                                                    const struct pipe_resource *tmpl)
{
    struct axiome_winsys_screen *ws = axiome_screen(pscreen);
    struct axiome_winsys_resource *res = CALLOC_STRUCT(axiome_winsys_resource);
    if (!res)
        return NULL;
    res->base = *tmpl;
    res->base.screen = pscreen;
    pipe_reference_init(&res->base.reference, 1);

    if (tmpl->bind & PIPE_BIND_DISPLAY_TARGET) {
        /* Allocate a dumb buffer via axdri_create for scanout/frontbuffer. */
        if (ws->nr_dumb >= AXIOME_MAX_DUMB) {
            FREE(res);
            return NULL;
        }
        struct axdri_buffer *buf = &ws->dumb[ws->nr_dumb].buf;
        buf->width = tmpl->width0;
        buf->height = tmpl->height0;
        if (axdri_create(ws->fd, buf) < 0) {
            FREE(res);
            return NULL;
        }
        ws->dumb[ws->nr_dumb].cpu_ptr = NULL;
        ws->dumb[ws->nr_dumb].buf.handle = buf->handle;
        res->dumb = &ws->dumb[ws->nr_dumb];
        ws->nr_dumb++;
    }
    return &res->base.base;
}

static void axiome_resource_destroy(struct pipe_screen *pscreen,
                                    struct pipe_resource *pres)
{
    struct axiome_winsys_resource *res = axiome_resource(pres);
    if (res->dumb)
        axdri_destroy(((struct axiome_winsys_screen *)pscreen)->fd,
                      &res->dumb->buf);
    FREE(res);
}

static int axiome_resource_get_handle(struct pipe_screen *pscreen,
                                      struct pipe_context *pcontext,
                                      struct pipe_resource *pres,
                                      struct winsys_handle *whandle,
                                      unsigned usage)
{
    /* No handle sharing yet. */
    return 0;
}

static void axiome_present_screen(struct pipe_context *pcontext,
                                  struct pipe_resource *pres,
                                  void *ctx_private,
                                  struct pipe_box *box)
{
    /* Present via axdri_present when a display-target resource is drawn. */
}

static const struct pipe_screen_vtable axiome_screen_vtable = {
    .destroy = axiome_destroy_screen,
    .get_name = axiome_get_name,
    .get_param = axiome_get_param,
    .get_paramf = axiome_get_paramf,
    .get_shader_param = axiome_get_shader_param,
    .is_format_supported = axiome_is_format_supported,
    .context_create = axiome_create_context,
    .create_surface = axiome_create_surface,
    .surface_destroy = axiome_surface_destroy,
    .resource_create = axiome_resource_create,
    .resource_destroy = axiome_resource_destroy,
    .transfer_map = axiome_transfer_map,
    .transfer_flush = axiome_transfer_flush,
    .transfer_unmap = axiome_transfer_unmap,
    .flush = axiome_flush,
    .clear = axiome_clear,
    .draw_vbo = axiome_draw_vbo,
    .get_handle = axiome_resource_get_handle,
    .present_screen = axiome_present_screen,
};

struct pipe_screen *axiome_create_screen(int fd)
{
    struct axiome_winsys_screen *ws = CALLOC_STRUCT(axiome_winsys_screen);
    if (!ws)
        return NULL;
    ws->fd = fd;
    ws->nr_dumb = 0;

    struct axdri_mode mode;
    if (axdri_get_mode(fd, &mode) < 0) {
        ws->width = 1024;
        ws->height = 768;
        ws->pitch = 4096;
    } else {
        ws->width = mode.width;
        ws->height = mode.height;
        ws->pitch = mode.pitch;
    }

    ws->base.destroy = axiome_screen_vtable.destroy;
    ws->base.get_name = axiome_screen_vtable.get_name;
    ws->base.get_param = axiome_screen_vtable.get_param;
    ws->base.get_paramf = axiome_screen_vtable.get_paramf;
    ws->base.get_shader_param = axiome_screen_vtable.get_shader_param;
    ws->base.is_format_supported = axiome_screen_vtable.is_format_supported;
    ws->base.context_create = axiome_screen_vtable.context_create;
    ws->base.create_surface = axiome_screen_vtable.create_surface;
    ws->base.surface_destroy = axiome_screen_vtable.surface_destroy;
    ws->base.resource_create = axiome_screen_vtable.resource_create;
    ws->base.resource_destroy = axiome_screen_vtable.resource_destroy;
    ws->base.transfer_map = axiome_screen_vtable.transfer_map;
    ws->base.transfer_flush = axiome_screen_vtable.transfer_flush;
    ws->base.transfer_unmap = axiome_screen_vtable.transfer_unmap;
    ws->base.flush = axiome_screen_vtable.flush;
    ws->base.clear = axiome_screen_vtable.clear;
    ws->base.draw_vbo = axiome_screen_vtable.draw_vbo;
    ws->base.get_handle = axiome_screen_vtable.get_handle;
    ws->base.present_screen = axiome_screen_vtable.present_screen;

    return &ws->base;
}

struct pipe_context *axiome_create_context(struct pipe_screen *screen,
                                            void *priv, unsigned flags)
{
    struct axiome_winsys_screen *ws = axiome_screen(screen);
    struct axiome_winsys_context *ctx = CALLOC_STRUCT(axiome_winsys_context);
    if (!ctx)
        return NULL;
    ctx->screen = screen;
    ctx->base.screen = screen;
    ctx->base.destroy = axiome_destroy_context;
    ctx->base.flush = axiome_flush;
    ctx->base.clear = axiome_clear;
    ctx->base.draw_vbo = axiome_draw_vbo;
    ctx->base.create_surface = axiome_create_surface;
    ctx->base.surface_destroy = axiome_surface_destroy;
    ctx->base.transfer_map = axiome_transfer_map;
    ctx->base.transfer_flush = axiome_transfer_flush;
    ctx->base.transfer_unmap = axiome_transfer_unmap;
    ctx->base.resource_create = NULL; /* handled by screen */
    ctx->base.resource_destroy = NULL;
    ctx->softpipe_ctx = NULL; /* softpipe wraps this later */
    return &ctx->base;
}
