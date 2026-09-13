/* Unified GUI input queue + /Devices/input0.
   Producers (PS/2 keyboard, USB-HID keyboard/mouse, PS/2 mouse) run in IRQ
   or poll context, so the queue is a fixed ring with no allocation and no
   sleeping. Consumers are GUI programs (axwm, axterm): read() drains whole
   events, non-blocking (0 when empty) so a compositor can poll each frame.
   The GUI grab flag (write 1/0) tells the keyboard paths to skip the TTY
   line discipline while a desktop holds the screen. */

#include "input.h"
#include "input_abi.h"
#include "driver.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "spinlock.h"
#include "framebuffer.h"
#include <stddef.h>

#define INPUT_DEPTH 256

static struct axinput_event g_queue[INPUT_DEPTH];
static volatile int g_head, g_tail;
static spinlock_t g_lock = SPINLOCK_INIT;
static volatile int g_grabbed;

static void input_push(struct axinput_event ev)
{
    unsigned long flags = spin_lock_irq(&g_lock);
    int next = (g_head + 1) % INPUT_DEPTH;
    if (next != g_tail)
    {
        g_queue[g_head] = ev;
        g_head = next;
    }
    spin_unlock_irq(&g_lock, flags);
}

void input_push_key(uint32_t code)
{
    struct axinput_event ev;
    ev.type = AXINPUT_TYPE_KEY;
    ev.code = code;
    ev.dx = 0;
    ev.dy = 0;
    input_push(ev);
}

void input_push_mouse(int dx, int dy, uint32_t buttons)
{
    struct axinput_event ev;
    ev.type = AXINPUT_TYPE_MOUSE;
    ev.code = buttons;
    ev.dx = dx;
    ev.dy = dy;
    input_push(ev);
}

int input_gui_grabbed(void)
{
    return g_grabbed;
}

static long input_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    (void)d;
    (void)off;
    if (len < sizeof(struct axinput_event))
        return 0;
    size_t max = len / sizeof(struct axinput_event);
    struct axinput_event *out = (struct axinput_event *)buf;
    size_t got = 0;
    unsigned long flags = spin_lock_irq(&g_lock);
    while (got < max && g_tail != g_head)
    {
        out[got++] = g_queue[g_tail];
        g_tail = (g_tail + 1) % INPUT_DEPTH;
    }
    spin_unlock_irq(&g_lock, flags);
    return (long)(got * sizeof(struct axinput_event));
}

static long input_write(struct device *d, uint64_t off, const void *buf,
                        size_t len)
{
    (void)d;
    (void)off;
    if (len < sizeof(uint32_t) || !buf)
        return -1;
    uint32_t v = *(const volatile uint32_t *)buf;
    int was = g_grabbed;
    g_grabbed = (v != 0);
    printk("INPUT: gui %s\n", g_grabbed ? "grab" : "release");
    if (was && !g_grabbed)
    {
        /* Hand a clean text console back: the desktop's pixels are still in
           the staging buffer, and any stray glyphs queued mid-frame would
           otherwise linger as glitch artefacts. */
        fb_clear();
    }
    return (long)len;
}

static struct dev_ops input_ops = {
    .read = input_read,
    .write = input_write,
};

void input_init(void)
{
    if (device_find("input0"))
        return;
    struct device *d = (struct device *)kmalloc(sizeof(struct device));
    if (!d)
        return;
    __builtin_memset(d, 0, sizeof(*d));
    const char *n = "input0";
    for (int i = 0; n[i] && i < 31; i++)
        d->name[i] = n[i];
    d->major = 227;
    d->minor = 0;
    d->type = DEV_CHAR;
    d->ops = input_ops;
    device_register(d);
    printk("DRV: input0 -> /Devices/input0 (GUI keyboard+mouse)\n");
}
