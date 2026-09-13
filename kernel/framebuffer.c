#include "framebuffer.h"
#include "font8x16.h"
#include "arch_mmu.h"
#include "pmm.h"
#include "input.h"
#include <stddef.h>

static struct {
    volatile uint8_t *addr;
    uint8_t *back_buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t type;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t num_cols;
    uint32_t num_rows;
    uint32_t fg_color;
    uint32_t bg_color;
    int present;
    int dirty;
    uint32_t dirty_x;
    uint32_t dirty_y;
    uint32_t dirty_w;
    uint32_t dirty_h;
} fb;

static inline uint8_t *fb_ptr(void)
{
    return fb.back_buffer ? fb.back_buffer : (uint8_t *)fb.addr;
}

/* WC buffers retire on full fill or on SFENCE. Without a fence the last
   partial 64-byte buffer can sit in the CPU indefinitely, so every burst
   of WC stores must close with one. */
static inline void fb_sfence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

void fb_init(uintptr_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp, uint8_t type)
{
    fb.addr = (volatile uint8_t *)addr;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = bpp;
    fb.type = type;
    fb.cursor_x = 0;
    fb.cursor_y = 0;
    fb.fg_color = 0x00FFFFFF;
    fb.bg_color = 0x00000000;
    fb.back_buffer = 0;
    /* Keep present=0 while remapping: mmu_map_framebuffer() runs before
       vmm_init() and must not let a concurrent printk recurse into
       fb_putchar() with fb.addr still pointing at the unmapped phys addr
       (that #PF with only the boot null-IDT installed triple-faults). */
    fb.present = 0;
    fb.num_cols = width / FONT_WIDTH;
    fb.num_rows = height / FONT_HEIGHT;
    fb.dirty = 0;
    fb.dirty_w = 0;

    if (addr != 0 && width > 0 && height > 0)
    {
        /* mmu_map_framebuffer() maps this WC (PAT index 4) so pixel stores
           burst over PCIe instead of read-modify-writing per cache line. */
        fb.addr = (volatile uint8_t *)mmu_map_framebuffer(
            addr, (size_t)height * pitch);
        if (fb.addr)
        {
            __builtin_memset((void *)fb.addr, 0, (size_t)height * pitch);
            fb_sfence();
            fb.present = 1;
        }
    }
}

void fb_init_buffers(void)
{
    if (!fb.present)
        return;
    if (fb.back_buffer)
        return;

    size_t fb_size = (size_t)fb.pitch * fb.height;
    size_t num_frames = (fb_size + 0xFFF) >> 12;
    void *phys = pmm_alloc_frames(num_frames);
    if (!phys)
        return;

    fb.back_buffer = (uint8_t *)(uintptr_t)phys;
    __builtin_memset(fb.back_buffer, 0, fb_size);
}

int fb_active(void)
{
    return fb.present;
}

uint32_t fb_width(void)  { return fb.width; }
uint32_t fb_height(void) { return fb.height; }
uint32_t fb_pitch(void)  { return fb.pitch; }
volatile void *fb_addr(void) { return fb.addr; }
uint8_t *fb_back_buffer_for_gfx(void) { return fb.back_buffer; }

static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return;

    uint32_t new_end_x = x + w;
    uint32_t new_end_y = y + h;

    if (fb.dirty_w == 0)
    {
        fb.dirty_x = x;
        fb.dirty_y = y;
        fb.dirty_w = w;
        fb.dirty_h = h;
    }
    else
    {
        if (x < fb.dirty_x) fb.dirty_x = x;
        if (y < fb.dirty_y) fb.dirty_y = y;
        if (new_end_x > fb.dirty_x + fb.dirty_w)
            fb.dirty_w = new_end_x - fb.dirty_x;
        if (new_end_y > fb.dirty_y + fb.dirty_h)
            fb.dirty_h = new_end_y - fb.dirty_y;
    }
    fb.dirty = 1;
}

static void set_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= fb.width || y >= fb.height)
        return;

    /* The GOP path always hands us 32bpp (see bootloader/gop.c), but be
       defensive: a future mode (24bpp) must not overflow into the next
       pixel. Only 32bpp is accelerated; other depths fall back to a byte
       writer. */
    uint8_t *p = fb_ptr() + (size_t)y * fb.pitch + (size_t)x * (fb.bpp / 8);
    if (fb.bpp == 32)
    {
        p[0] = (uint8_t)(color >> 0);
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
        p[3] = (uint8_t)(color >> 24);
    }
    else if (fb.bpp == 24)
    {
        p[0] = (uint8_t)(color >> 0);
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    }
    else
    {
        /* Unknown depth: best-effort 32-bit store, clipped to the row. */
        size_t row_left = (size_t)fb.pitch - (size_t)x * (fb.bpp / 8);
        size_t n = row_left < 4 ? row_left : 4;
        for (size_t i = 0; i < n; i++)
            p[i] = (uint8_t)(color >> (i * 8));
    }
}

static void draw_glyph(char c, uint32_t cell_x, uint32_t cell_y, uint32_t fg, uint32_t bg)
{
    if (cell_x >= fb.num_cols || cell_y >= fb.num_rows)
        return;

    unsigned int idx = (unsigned char)c - FONT_FIRST_CHAR;
    if (idx >= FONT_NUM_CHARS)
        return;

    uint32_t base_x = cell_x * FONT_WIDTH;
    uint32_t base_y = cell_y * FONT_HEIGHT;

    for (int row = 0; row < FONT_HEIGHT; row++)
    {
        uint8_t bits = font8x16[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++)
        {
            uint32_t color = (bits & (1 << (7 - col))) ? fg : bg;
            set_pixel(base_x + col, base_y + row, color);
        }
    }

    mark_dirty(base_x, base_y, FONT_WIDTH, FONT_HEIGHT);
}

static void __attribute__((unused)) fill_row(uint32_t row, uint32_t color)
{
    if (row >= fb.num_rows)
        return;
    uint8_t *ptr = fb_ptr();
    if (fb.bpp == 32)
    {
        for (uint32_t y = row * FONT_HEIGHT; y < (row + 1) * FONT_HEIGHT; y++)
        {
            uint32_t *line = (uint32_t *)(ptr + (size_t)y * fb.pitch);
            for (uint32_t x = 0; x < fb.width; x++)
                line[x] = color;
        }
    }
    else
    {
        /* Generic depth: paint pixel-by-pixel so pitch padding and short
           pixels are never overrun. Slower, but only used on non-32bpp. */
        for (uint32_t y = row * FONT_HEIGHT; y < (row + 1) * FONT_HEIGHT; y++)
            for (uint32_t x = 0; x < fb.width; x++)
                set_pixel(x, y, color);
    }
    mark_dirty(0, row * FONT_HEIGHT, fb.width, FONT_HEIGHT);
}

static void fill_lines(uint32_t y0, uint32_t y1, uint32_t color)
{
    /* Fill scanlines [y0, y1) with a solid color, honouring pitch. */
    if (y0 >= fb.height)
        return;
    if (y1 > fb.height)
        y1 = fb.height;
    if (y0 >= y1)
        return;
    uint8_t *ptr = fb_ptr();
    if (fb.bpp == 32 && color == 0)
    {
        __builtin_memset(ptr + (size_t)y0 * fb.pitch, 0,
                         (size_t)(y1 - y0) * fb.pitch);
        return;
    }
    if (fb.bpp == 32)
    {
        for (uint32_t y = y0; y < y1; y++)
        {
            uint32_t *line = (uint32_t *)(ptr + (size_t)y * fb.pitch);
            for (uint32_t x = 0; x < fb.width; x++)
                line[x] = color;
        }
        return;
    }
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < fb.width; x++)
            set_pixel(x, y, color);
}

static void shift_rows_up(void)
{
    uint8_t *ptr = fb_ptr();
    uint32_t scroll_h = (fb.num_rows - 1) * FONT_HEIGHT;

    /* Overlapping rows: dst < src, must be memmove (memcpy is UB and gcc
       may vectorise it backwards, smearing glyphs). The kernel is
       freestanding with no memmove, so copy forward byte-wise: dst < src
       makes forward iteration safe. */
    for (uint32_t y = 0; y < scroll_h; y++)
    {
        uint8_t *dst = ptr + (size_t)y * fb.pitch;
        const uint8_t *src = ptr + (size_t)(y + FONT_HEIGHT) * fb.pitch;
        for (size_t i = 0; i < fb.pitch; i++)
            dst[i] = src[i];
    }
    /* Clear the freed text row AND any sub-glyph remainder strip below the
       text grid (e.g. 600px height with 16px font leaves 8px that the old
       code never scrolled or dirtied, leaving "uncleared" bars). */
    fill_lines(scroll_h, fb.height, fb.bg_color);
    /* One full-width dirty rect covers the scroll + clear, so fb_flush()
       can never leave the remainder strip stale. */
    fb.dirty = 0;
    fb.dirty_w = 0;
    mark_dirty(0, 0, fb.width, fb.height);
}

void fb_flush(void)
{
    if (!fb.present || !fb.back_buffer || !fb.dirty)
        return;

    /* Copy only the dirty rectangle: a full-screen copy per glyph would
       push megabytes over PCIe for every character. WC stores burst per
       cache line; SFENCE retires the trailing partial buffer. */
    uint32_t bpp_bytes = (uint32_t)(fb.bpp / 8);
    if (bpp_bytes == 0)
        bpp_bytes = 4;
    uint32_t x0 = fb.dirty_x;
    uint32_t y0 = fb.dirty_y;
    uint32_t w = fb.dirty_w;
    uint32_t h = fb.dirty_h;

    if (x0 >= fb.width || y0 >= fb.height)
    {
        fb.dirty = 0;
        fb.dirty_w = 0;
        return;
    }
    if (x0 + w > fb.width)
        w = fb.width - x0;
    if (y0 + h > fb.height)
        h = fb.height - y0;

    uint8_t *dst_base = (uint8_t *)fb.addr;
    uint8_t *src_base = fb.back_buffer;
    size_t row_bytes = (size_t)w * bpp_bytes;
    size_t x_off = (size_t)x0 * bpp_bytes;
    for (uint32_t y = y0; y < y0 + h; y++)
        __builtin_memcpy(dst_base + (size_t)y * fb.pitch + x_off,
                         src_base + (size_t)y * fb.pitch + x_off,
                         row_bytes);
    fb_sfence();

    fb.dirty = 0;
    fb.dirty_w = 0;
}

void fb_putchar(char c)
{
    if (!fb.present)
        return;
    /* A GUI desktop owns the screen while the input grab is held: its dumb
       buffer is the scanout source, so console glyphs would only flicker
       for one frame before the next PRESENT overwrites them. Stay quiet on
       the display (serial + klog keep logging). */
    if (input_gui_grabbed())
        return;

    switch (c)
    {
        case '\n':
            fb.cursor_x = 0;
            fb.cursor_y++;
            break;
        case '\r':
            fb.cursor_x = 0;
            break;
        case '\t':
            fb.cursor_x = (fb.cursor_x + 8) & ~7;
            break;
        case '\b':
            if (fb.cursor_x > 0)
                fb.cursor_x--;
            break;
        default:
            draw_glyph(c, fb.cursor_x, fb.cursor_y, fb.fg_color, fb.bg_color);
            fb.cursor_x++;
            break;
    }

    if (fb.cursor_x >= fb.num_cols)
    {
        fb.cursor_x = 0;
        fb.cursor_y++;
    }

    while (fb.cursor_y >= fb.num_rows)
    {
        shift_rows_up();
        fb.cursor_y--;
    }
}

void fb_write(const char *s)
{
    if (!fb.present || !s)
        return;
    if (!*s)
        return;
    while (*s)
        fb_putchar(*s++);
    if (fb.back_buffer)
        fb_flush();
    else
        fb_sfence();
}

void fb_scroll(void)
{
    if (!fb.present)
        return;
    shift_rows_up();
    fb.cursor_y = fb.num_rows - 1;
    if (fb.back_buffer)
        fb_flush();
    else
        fb_sfence();
}

void fb_clear(void)
{
    if (!fb.present)
        return;
    /* Honour bg_color (fast memset when black). The old code always cleared
       to zero bytes, so a non-black bg left "uncleared" coloured streaks
       after scroll vs clear. */
    fill_lines(0, fb.height, fb.bg_color);
    fb.cursor_x = 0;
    fb.cursor_y = 0;
    mark_dirty(0, 0, fb.width, fb.height);
    if (fb.back_buffer)
        fb_flush();
    else
        fb_sfence();
}

void fb_set_color(uint32_t fg, uint32_t bg)
{
    fb.fg_color = fg;
    fb.bg_color = bg;
}

void fb_get_color(uint32_t *fg, uint32_t *bg)
{
    if (fg) *fg = fb.fg_color;
    if (bg) *bg = fb.bg_color;
}

void fb_set_cursor(uint32_t x, uint32_t y)
{
    if (x < fb.num_cols) fb.cursor_x = x;
    if (y < fb.num_rows) fb.cursor_y = y;
}

void fb_get_cursor(uint32_t *x, uint32_t *y)
{
    if (x) *x = fb.cursor_x;
    if (y) *y = fb.cursor_y;
}

void fb_putchar_at(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
    if (!fb.present)
        return;
    if (input_gui_grabbed())
        return;
    draw_glyph(c, x, y, fg, bg);
    if (fb.back_buffer)
        fb_flush();
    else
        fb_sfence();
}

void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!fb.present || w == 0 || h == 0)
        return;
    if (x >= fb.width || y >= fb.height)
        return;
    if (x + w > fb.width)
        w = fb.width - x;
    if (y + h > fb.height)
        h = fb.height - y;
    mark_dirty(x, y, w, h);
}
