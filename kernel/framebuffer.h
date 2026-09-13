#ifndef AXIOME_FRAMEBUFFER_H
#define AXIOME_FRAMEBUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void fb_init(uintptr_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp, uint8_t type);
void fb_init_buffers(void);
void fb_putchar(char c);
void fb_write(const char *s);
void fb_scroll(void);
void fb_clear(void);
void fb_flush(void);
int fb_active(void);

void fb_set_color(uint32_t fg, uint32_t bg);
void fb_get_color(uint32_t *fg, uint32_t *bg);
void fb_set_cursor(uint32_t x, uint32_t y);
void fb_get_cursor(uint32_t *x, uint32_t *y);
void fb_putchar_at(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

/* Mark a pixel rectangle dirty so the next fb_flush()/gfx_present() copies
   it to scanout. Direct pixel writers that bypass fb_putchar (the gfx
   backends, DRI PRESENT) must call this after scribbling on the staging
   buffer; otherwise the flush sees a clean flag and the pixels stay
   invisible. Coordinates are clamped to the live mode. */
void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);
volatile void *fb_addr(void);

/* CPU-visible staging buffer for the gfx abstraction (GopDisplay) and the
   Mesa softpipe target. NULL until fb_init_buffers() succeeds. */
uint8_t *fb_back_buffer_for_gfx(void);

#ifdef __cplusplus
}
#endif

#endif
