#include "mouse.h"
#include "printk.h"
#include "io.h"
#include "input.h"
#include "hal/cshim.h"
#include "spinlock.h"

#define MOUSE_IRQ 12

#define CMD_PORT 0x64
#define DATA_PORT 0x60

#define CMD_WRITE_AUX 0xD4
#define CMD_AUX_ENABLE 0xA8

#define AUX_SET_DEFAULTS 0xF6
#define AUX_ENABLE_DATA 0xF4
#define AUX_SET_SAMPLE 0xF3
#define AUX_SET_RES 0xE8

#define MOUSE_EVENT_BUF 32

static struct mouse_event evbuf[MOUSE_EVENT_BUF];
static volatile int evhead, evtail;
static spinlock_t evlock = SPINLOCK_INIT;
static uint8_t mouse_cycle;
static uint8_t mouse_packet[3];

static inline void wait_write(void)
{
    for (int i = 0; i < 10000; i++)
        if (!(inb(CMD_PORT) & 2))
            return;
}

static void aux_write(uint8_t val)
{
    wait_write();
    outb(CMD_PORT, CMD_WRITE_AUX);
    wait_write();
    outb(DATA_PORT, val);
}

static uint8_t aux_read(void)
{
    for (int i = 0; i < 10000; i++)
        if (inb(CMD_PORT) & 1)
            return inb(DATA_PORT);
    return 0;
}

void mouse_submit_event(int dx, int dy, uint8_t buttons)
{
    /* Always mirror into the GUI input queue (in addition to the legacy
       polled buffer) so axwm/axterm see PS/2 and USB mice uniformly. */
    input_push_mouse(dx, dy, (uint32_t)(buttons & 7));
    unsigned long flags = spin_lock_irq(&evlock);
    int next = (evhead + 1) % MOUSE_EVENT_BUF;
    if (next != evtail)
    {
        evbuf[evhead].dx = dx;
        evbuf[evhead].dy = dy;
        evbuf[evhead].buttons = buttons;
        evhead = next;
    }
    spin_unlock_irq(&evlock, flags);
}

static void mouse_process_packet(void)
{
    if ((mouse_packet[0] & 0x08) == 0 || (mouse_packet[0] & 0xc0) != 0)
        return;
    /* PS/2 motion bytes are Cartesian (+up), the screen is not: negate Y so
       the queue carries screen-oriented deltas (+right, +down). Verified
       against QEMU's PS/2 emulation (monitor mouse_move 0 25 -> dy=+25). */
    mouse_submit_event((int)(int8_t)mouse_packet[1],
                       -(int)(int8_t)mouse_packet[2], mouse_packet[0] & 7);
}

void mouse_irq_handler(void)
{
    uint8_t status = inb(CMD_PORT);
    if (status & 0x20)
    {
        uint8_t data = inb(DATA_PORT);
        mouse_packet[mouse_cycle] = data;
        mouse_cycle++;
        if (mouse_cycle >= 3)
        {
            mouse_cycle = 0;
            mouse_process_packet();
        }
    }
}

void mouse_init(void)
{
    printk("Mouse: probing...\n");

    outb(CMD_PORT, CMD_AUX_ENABLE);
    aux_write(AUX_SET_DEFAULTS);
    aux_read();

    aux_write(AUX_SET_SAMPLE);
    aux_read();
    aux_write(100);
    aux_read();

    aux_write(AUX_ENABLE_DATA);
    uint8_t ack = aux_read();
    if (ack != 0xFA)
    {
        printk("Mouse: no response (0x%x)\n", ack);
        return;
    }

    hal_irq_mask(MOUSE_IRQ, 0);
    printk("Mouse: ready\n");
}

int mouse_read_event(struct mouse_event *ev)
{
    unsigned long flags = spin_lock_irq(&evlock);
    if (evtail == evhead) {
        spin_unlock_irq(&evlock, flags);
        return 0;
    }
    *ev = evbuf[evtail];
    evtail = (evtail + 1) % MOUSE_EVENT_BUF;
    spin_unlock_irq(&evlock, flags);
    return 1;
}
