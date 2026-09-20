#include "xhci.h"

#include "acpi.h"
#include "apic.h"
#include "clock.h"
#include "driver.h"
#include "hal/cshim.h"
#include "ide.h"
#include "idt.h"
#include "ioapic.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "pmm.h"
#include "printk.h"
#include "sched.h"
#include "slab.h"
#include "string.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

#define XHCI_MAX_PORTS 16
#define XHCI_RING_TRBS 256
#define XHCI_TIMEOUT 20000000u

#define PORTSC_CCS (1u<<0)
#define PORTSC_PED (1u<<1)
#define PORTSC_PR (1u<<4)
#define PORTSC_PP (1u<<9)
#define PORTSC_CSC (1u<<17)
#define PORTSC_PEC (1u<<18)
#define PORTSC_WRC (1u<<19)
#define PORTSC_OCC (1u<<20)
#define PORTSC_PRC (1u<<21)
#define PORTSC_PLC (1u<<22)
#define PORTSC_CEC (1u<<23)
#define PORTSC_WPR (1u<<31)
#define PORTSC_CHANGE_MASK (PORTSC_CSC|PORTSC_PEC|PORTSC_WRC|PORTSC_OCC|PORTSC_PRC|PORTSC_PLC|PORTSC_CEC)
#define TRB_TC (1u<<1) /* Link TRB Toggle Cycle */

#define TRB_TYPE(n) ((uint32_t)(n) << 10)
#define TRB_CYCLE 1u
#define TRB_ENT (1u << 1)
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_DIR_IN (1u << 16)

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct xhci_dma {
    void *virt;
    uint64_t phys;
    uint32_t pages;
};

struct xhci_ring {
    struct xhci_dma dma;
    volatile struct xhci_trb *trb;
    uint16_t enqueue;
    uint8_t cycle;
};

#define DEV_TYPE_NONE 0
#define DEV_TYPE_HID 1
#define DEV_TYPE_MSC 2

/* BOT / SCSI */
struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} __attribute__((packed));

struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

struct xhci_device {
    int active;
    int dev_type;
    int pending;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint8_t protocol;
    uint8_t configuration;
    uint8_t interface;
    uint8_t endpoint; /* HID interrupt endpoint number */
    uint8_t interval;
    uint16_t max_packet;
    uint16_t report_size;
    uint64_t pending_trb;
    struct xhci_dma input_ctx;
    struct xhci_dma device_ctx;
    struct xhci_dma ep0_data;
    struct xhci_dma report;
    struct xhci_ring ep0;
    struct xhci_ring intr;
    /* MSC bulk endpoints */
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    struct xhci_ring bulk_in;
    struct xhci_ring bulk_out;
    struct xhci_dma msc_cbw;
    struct xhci_dma msc_data;
    struct xhci_dma msc_csw;
    uint32_t block_size;
    uint64_t block_count;
    uint32_t msc_tag;
    struct block_dev blk;
    struct device *blk_dev;
    struct xhci_controller *ctrl;
};

struct xhci_controller {
    int ready;
    volatile uint8_t *mmio;
    volatile uint8_t *op;
    volatile uint8_t *runtime;
    volatile uint32_t *doorbell;
    uint8_t cap_length;
    uint8_t context_size;
    uint8_t max_slots;
    uint8_t max_ports;
    uint16_t cmd_enqueue;
    uint8_t cmd_cycle;
    uint16_t event_dequeue;
    uint8_t event_cycle;
    struct xhci_dma dcbaa;
    struct xhci_dma scratch_array;
    struct xhci_dma scratch[32];
    struct xhci_dma command;
    struct xhci_dma event;
    struct xhci_dma erst;
    struct xhci_device devices[XHCI_MAX_PORTS];
    uint8_t irq_vector;
    uint8_t irq_gsi;
    int irq_enabled;
    volatile uint32_t irq_pending;
};

#define XHCI_MAX_CONTROLLERS 4
static struct xhci_controller hcs[XHCI_MAX_CONTROLLERS];
static int n_hcs;
static struct xhci_controller *cur_hc;
#define hc (*cur_hc)

static inline uint32_t rd32(volatile void *p)
{
    return *(volatile uint32_t *)p;
}

static inline void wr32(volatile void *p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
}

static inline void wr64(volatile void *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32((volatile uint8_t *)p + 4, (uint32_t)(v >> 32));
}

static void barrier(void)
{
    __sync_synchronize();
}

/* Hybrid delay: uses clock_mono_ns if timer is live, otherwise busy pause.
   Replaces the old 300k-iteration busy loop which was inaccurate and blocked
   the scheduler. Mirrors Haiku's snooze() behavior. */
static void xhci_mdelay(uint32_t ms)
{
    if (ms == 0)
        return;
    /* Early boot before clock_init or before IRQs enabled: clock_mono_ns()
       returns 0 and never advances – use busy loop. Also avoid sched_sleep
       before scheduler/IRQs are alive (kernel.c: pci probes before clock_init
       and before hal_cpu_irq_enable). */
    uint64_t start = clock_mono_ns();
    if (start == 0 || apic_get_ticks() == 0) {
        for (uint32_t i = 0; i < ms * 300000u; i++) {
            hal_cpu_pause();
            barrier();
        }
        return;
    }
    uint64_t ns = (uint64_t)ms * 1000000ULL;
    uint64_t deadline = start + ns;
    while (clock_mono_ns() < deadline) {
        /* If scheduler is alive, sleep to let other threads run. */
        if (sched_current() && ms >= 5) {
            uint64_t remain = deadline - clock_mono_ns();
            if (remain > 1000000ULL)
                remain = 1000000ULL;
            sched_sleep_ns(remain);
        } else {
            hal_cpu_pause();
            barrier();
        }
        if (clock_mono_ns() == start) /* clock stalled */
            break;
    }
}

static void __attribute__((unused)) xhci_udelay(uint32_t us)
{
    uint64_t start = clock_mono_ns();
    if (start == 0 || apic_get_ticks() == 0) {
        for (uint32_t i = 0; i < us * 80u; i++) {
            hal_cpu_pause();
            barrier();
        }
        return;
    }
    uint64_t deadline = start + (uint64_t)us * 1000ULL;
    while (clock_mono_ns() < deadline) {
        hal_cpu_pause();
        barrier();
    }
}

static int wait32(volatile void *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT; i++) {
        if ((rd32(reg) & mask) == value)
            return 0;
        if ((i & 0xFF) == 0)
            barrier();
    }
    return -1;
}

static int dma_alloc(struct xhci_dma *dma, uint32_t pages)
{
    void *phys = pmm_alloc_frames(pages);
    if (!phys)
        return -1;
    void *virt = vmm_mmap_phys((uint64_t)(uintptr_t)phys, pages,
                               MMU_WRITE | MMU_UNCACHED);
    if (!virt) {
        pmm_free_frames(phys, pages);
        return -1;
    }
    dma->virt = virt;
    dma->phys = (uint64_t)(uintptr_t)phys;
    dma->pages = pages;
    memset(virt, 0, (size_t)pages * PAGE_SIZE);
    return 0;
}

static int ring_alloc(struct xhci_ring *ring)
{
    if (dma_alloc(&ring->dma, 1) < 0)
        return -1;
    ring->trb = (volatile struct xhci_trb *)ring->dma.virt;
    ring->cycle = 1;
    ring->enqueue = 0;
    return 0;
}

static uint64_t ring_push(struct xhci_ring *ring, uint64_t parameter,
                          uint32_t status, uint32_t control)
{
    uint16_t index = ring->enqueue;
    volatile struct xhci_trb *trb = &ring->trb[index];
    trb->parameter = parameter;
    trb->status = status;
    barrier();
    trb->control = control | ring->cycle;
    ring->enqueue++;
    if (ring->enqueue == XHCI_RING_TRBS - 1) {
        volatile struct xhci_trb *link = &ring->trb[ring->enqueue];
        link->parameter = ring->dma.phys;
        link->status = 0;
        barrier();
        link->control = TRB_TYPE(6) | TRB_TC | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    barrier();
    return ring->dma.phys + (uint64_t)index * sizeof(struct xhci_trb);
}

static void legacy_handoff(struct xhci_controller *c, uint32_t hcc)
{
    uint32_t off = ((hcc >> 16) & 0xffffu) * 4u;
    while (off) {
        volatile uint8_t *ext = c->mmio + off;
        uint32_t cap = rd32(ext);
        uint8_t id = (uint8_t)cap;
        uint8_t next = (uint8_t)(cap >> 8);
        if (id == 1) {
            printk("xHCI: BIOS handoff cap at 0x%x val 0x%x\n", off, cap);
            wr32(ext, cap | (1u << 24));
            for (int i = 0; i < 20; i++) {
                xhci_mdelay(50);
                cap = rd32(ext);
                if (!(cap & (1u << 16)))
                    break;
                printk("xHCI: BIOS still owned %d/20\n", i+1);
            }
            if (cap & (1u << 16)) {
                printk("xHCI: BIOS won't release ownership (cap 0x%x)\n", cap);
            } else {
                printk("xHCI: legacy ownership acquired\n");
            }
            /* Force off BIOS owned flag (Haiku: eec & ~BIOSOWNED). */
            wr32(ext, cap & ~(1u << 16));
            /* Clear all SMI enables – some BIOSes freeze if SMIs remain
               armed when interrupts fire (Haiku XHCI_LEGCTLSTS logic). */
            uint32_t ctl = rd32(ext + 4);
            /* DISABLE_SMI mask = (0x3<<1)|(0xff<<5)|(0x7<<17); clear those bits */
            ctl &= ~((0x3u << 1) | (0xffu << 5) | (0x7u << 17));
            wr32(ext + 4, ctl);
            (void)rd32(ext + 4); /* flush */
            printk("xHCI: legacy SMI disabled, ctl=0x%x\n", ctl);
            return;
        }
        if (!next)
            break;
        off += (uint32_t)next * 4u;
    }
    printk("xHCI: no legacy cap found\n");
}

static volatile struct xhci_trb *event_current(struct xhci_controller *c)
{
    volatile struct xhci_trb *event =
        &((volatile struct xhci_trb *)c->event.virt)[c->event_dequeue];
    if ((event->control & TRB_CYCLE) != c->event_cycle)
        return 0;
    return event;
}

static void event_advance(struct xhci_controller *c)
{
    c->event_dequeue++;
    if (c->event_dequeue == XHCI_RING_TRBS) {
        c->event_dequeue = 0;
        c->event_cycle ^= 1;
    }
    volatile uint8_t *ir = c->runtime + 0x20;
    uint64_t erdp = c->event.phys + (uint64_t)c->event_dequeue * sizeof(struct xhci_trb);
    if (c->irq_enabled)
        erdp |= (1u << 3); /* EHB */
    wr64(ir + 0x18, erdp);
}

/* Acknowledge xHC interrupt: clear USBSTS EINT and IMAN IP per spec.
   Haiku does WriteOpReg(STS, ReadOpReg(STS)) and IMAN handling. */
static void xhci_ack_irq(struct xhci_controller *c)
{
    /* Clear Host Controller Error + Event Interrupt bits (W1C). */
    uint32_t sts = rd32(c->op + 4);
    wr32(c->op + 4, sts | (1u << 3)); /* STS_EINT = bit3 */
    /* Clear interrupter pending (IMAN IP bit 0 + IE bit1 stays). */
    volatile uint8_t *ir = c->runtime + 0x20;
    uint32_t iman = rd32(ir);
    wr32(ir, iman | 1u);
}

/* Interrupt handler – called via hal_irq_dispatch for vector 0x30-0x33.
   Drains event ring similarly to xhci_poll but also acks IRQ. */
static void xhci_irq_handler(void *ctx, void *frame)
{
    (void)frame;
    struct xhci_controller *c = (struct xhci_controller *)ctx;
    if (!c || !c->ready)
        goto eoi;
    /* Check if this HC actually interrupted (IMAN IP). */
    volatile uint8_t *ir = c->runtime + 0x20;
    uint32_t iman = rd32(ir);
    if (!(iman & 1u)) {
        /* Spurious – but still need EOI. */
        goto eoi;
    }
    xhci_ack_irq(c);
    c->irq_pending = 1;
    /* Wakeup polling: event ring will be drained by transfer_wait/command
       which already calls event_current. No extra work here to keep IRQ fast. */
eoi:
    hal_irq_eoi();
}

static const char *xhci_cc_string(uint8_t cc);

 /* Try to wire legacy pin IRQ via IOAPIC. Returns vector on success or -1. */
static int xhci_setup_irq(struct xhci_controller *c, struct pci_device *pdev)
{
    extern void isr_xhci0(void);
    extern void isr_xhci1(void);
    extern void isr_xhci2(void);
    extern void isr_xhci3(void);
    uintptr_t handlers[4] = { (uintptr_t)isr_xhci0, (uintptr_t)isr_xhci1, (uintptr_t)isr_xhci2, (uintptr_t)isr_xhci3 };
    int idx = -1;
    for (int i = 0; i < n_hcs; i++) {
        if (&hcs[i] == c) { idx = i; break; }
    }
    if (idx < 0 || idx >= 4)
        return -1;
    uint8_t vector = (uint8_t)(0x30 + idx);
    uintptr_t handler = handlers[idx];

    uint8_t pin_irq = pdev->irq;
    if (pin_irq == 0 || pin_irq == 0xFF) {
        printk("xHCI: no legacy IRQ for MSI fallback (irq=%u) – polling only\n", pin_irq);
        return -1;
    }
    uint32_t gsi = pin_irq;
    uint16_t flags = 0;
    if (acpi_iso_lookup(pin_irq, &gsi, &flags) != 0) {
        printk("xHCI: acpi_iso_lookup failed for irq %u, using gsi %u\n", pin_irq, gsi);
    }
    /* Install IDT gate */
    idt_set_gate(vector, handler, 0x8E, 0);
    /* Register HAL handler */
    if (hal_irq_register(vector, xhci_irq_handler, c) != 0) {
        printk("xHCI: hal_irq_register failed for vector 0x%x\n", vector);
        return -1;
    }
    /* Route GSI -> vector via IOAPIC (unmasked). */
    hal_irq_route((int)gsi, vector, 0);
    c->irq_vector = vector;
    c->irq_gsi = (uint8_t)gsi;
    c->irq_enabled = 1;
    c->irq_pending = 0;
    /* Now enable interrupter and HC INTE */
    {
        volatile uint8_t *ir = c->runtime + 0x20;
        wr32(ir, rd32(ir) | (1u << 1)); /* IMAN IE */
        uint32_t cmd = rd32(c->op);
        wr32(c->op, cmd | (1u << 2)); /* USBCMD INTE */
        printk("xHCI: IRQ enabled for controller %d\n", idx);
    }
    printk("xHCI: IRQ routed GSI %u -> vector 0x%x (pin irq %u) handler 0x%lx\n",
           gsi, vector, pin_irq, (unsigned long)handler);
    return vector;
}

static int command(struct xhci_controller *c, uint64_t parameter, uint32_t control, uint8_t *slot_out)
{
    uint16_t index = c->cmd_enqueue;
    volatile struct xhci_trb *trb =
        &((volatile struct xhci_trb *)c->command.virt)[index];
    uint64_t command_phys = c->command.phys +
                            (uint64_t)index * sizeof(struct xhci_trb);
    trb->parameter = parameter;
    trb->status = 0;
    barrier();
    uint32_t trb_ctrl = control | c->cmd_cycle;
    trb->control = trb_ctrl;
    c->cmd_enqueue++;
    if (c->cmd_enqueue == XHCI_RING_TRBS - 1) {
        volatile struct xhci_trb *link =
            &((volatile struct xhci_trb *)c->command.virt)[c->cmd_enqueue];
        link->parameter = c->command.phys;
        link->status = 0;
        barrier();
        link->control = TRB_TYPE(6) | TRB_TC | c->cmd_cycle;
        c->cmd_enqueue = 0;
        c->cmd_cycle ^= 1;
    }
    barrier();
    c->doorbell[0] = 0;

    uint64_t deadline = clock_mono_ns() + 1000ULL * 1000000ULL; /* 1s timeout */
    for (uint32_t guard = 0; guard < XHCI_TIMEOUT; guard++) {
        volatile struct xhci_trb *event = event_current(c);
        if (!event) {
            if (clock_mono_ns() > deadline)
                break;
            if ((guard & 0xFF) == 0) {
                hal_cpu_pause();
                if (c->irq_enabled && c->irq_pending) {
                    c->irq_pending = 0;
                    xhci_ack_irq(c);
                }
            }
            continue;
        }
        uint32_t type = (event->control >> 10) & 0x3f;
        uint64_t pointer = event->parameter & ~0xfULL;
        uint8_t completion = (uint8_t)(event->status >> 24);
        uint8_t slot = (uint8_t)(event->control >> 24);
        /* Ack IRQ if pending */
        if (c->irq_enabled) {
            xhci_ack_irq(c);
            c->irq_pending = 0;
        }
        event_advance(c);
        if (type == 33 && pointer == command_phys) {
            if (completion != 1)
                printk("xHCI: command failed cc=%u (%s) slot=%u param=0x%lx\n",
                       completion, xhci_cc_string(completion), slot, (unsigned long)pointer);
            if (slot_out)
                *slot_out = slot;
            return completion == 1 ? 0 : -(int)completion;
        }
        /* Ignore other events (port status, etc.) and continue */
        deadline = clock_mono_ns() + 1000ULL * 1000000ULL;
    }
    printk("xHCI: command timeout phys 0x%lx (%s)\n", (unsigned long)command_phys, "no completion within 1s");
    return -255;
}

static void handle_hid_event_inline(volatile struct xhci_trb *event);

static int transfer_wait(struct xhci_device *dev, uint64_t wanted,
                         uint32_t *residual)
{
    struct xhci_controller *c = dev->ctrl;
    if (!c) c = cur_hc;
    /* Short timeout for enumeration – long 2s caused painful retry loops (user report). */
    uint64_t start = clock_mono_ns();
    uint64_t deadline = 0;
    if (start != 0 && apic_get_ticks() != 0)
        deadline = start + 500ULL * 1000000ULL; /* 500ms */
    for (uint32_t guard = 0; guard < XHCI_TIMEOUT; guard++) {
        volatile struct xhci_trb *event = event_current(c);
        if (!event) {
            if (deadline != 0 && clock_mono_ns() > deadline)
                break;
            if ((guard & 0xFF) == 0) {
                hal_cpu_pause();
                if (c->irq_enabled && c->irq_pending) {
                    c->irq_pending = 0;
                    xhci_ack_irq(c);
                }
            }
            continue;
        }
        uint32_t type = (event->control >> 10) & 0x3f;
        uint64_t pointer = event->parameter & ~0xfULL;
        uint8_t completion = (uint8_t)(event->status >> 24);
        uint8_t slot = (uint8_t)(event->control >> 24);
        uint32_t remain = event->status & 0xffffffu;
        if (c->irq_enabled) {
            xhci_ack_irq(c);
            c->irq_pending = 0;
        }
        /* HID interrupt completions may arrive while we are waiting for a
           bulk or control transfer. Handle them inline so they are not lost
           and the interrupt ring stays primed. */
        if (type == 32) {
            int is_hid = 0;
            for (uint8_t i = 0; i < c->max_ports; i++) {
                struct xhci_device *hd = &c->devices[i];
                if (hd->active && hd->dev_type == DEV_TYPE_HID && hd->pending &&
                    hd->slot == slot && hd->pending_trb == pointer) {
                    is_hid = 1;
                    break;
                }
            }
            if (is_hid) {
                handle_hid_event_inline(event);
                event_advance(c);
                if (deadline != 0)
                    deadline = clock_mono_ns() + 500ULL * 1000000ULL;
                continue;
            }
        }
        if (type == 32 && slot == dev->slot && pointer == wanted) {
            if (residual)
                *residual = remain;
            event_advance(c);
            if (completion == 6) {
                printk("xHCI: transfer stalled slot %u (%s)\n", slot, xhci_cc_string(completion));
            }
            return (completion == 1 || completion == 13) ? 0 : -(int)completion;
        }
        /* Unexpected transfer event – log and ack */
        if (type == 32) {
            printk("xHCI: unexpected transfer cc=%u (%s) slot %u ptr 0x%lx wanted 0x%lx\n",
                   completion, xhci_cc_string(completion), slot, (unsigned long)pointer, (unsigned long)wanted);
        }
        event_advance(c);
        if (deadline != 0)
            deadline = clock_mono_ns() + 500ULL * 1000000ULL;
    }
    printk("xHCI: transfer timeout wanted 0x%lx dev slot %u\n", (unsigned long)wanted, dev->slot);
    return -255;
}

static uint64_t setup_value(uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, uint16_t length)
{
    return (uint64_t)request_type | ((uint64_t)request << 8) |
           ((uint64_t)value << 16) | ((uint64_t)index << 32) |
           ((uint64_t)length << 48);
}

static int control_transfer(struct xhci_device *dev, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            void *data, uint16_t length)
{
    int input = (request_type & 0x80) != 0;
    uint32_t trt = length ? (input ? 3u : 2u) : 0u;
    if (dev->ep0_data.phys + length > 0xFFFFFFFFULL) {
        printk("xHCI: control DMA beyond 32-bit (phys 0x%lx len %u)\n",
               (unsigned long)dev->ep0_data.phys, length);
        return -1;
    }
    /* Setup Stage – 8 bytes immediate (IDT). */
    ring_push(&dev->ep0, setup_value(request_type, request, value, index, length),
              8, TRB_TYPE(2) | (1u << 6) | TRB_CHAIN | (trt << 16));
    if (length) {
        if (!input && data)
            memcpy(dev->ep0_data.virt, data, length);
        ring_push(&dev->ep0, dev->ep0_data.phys, length,
                  TRB_TYPE(3) | (input ? TRB_DIR_IN : 0u) | TRB_CHAIN);
    }
    /* Status Stage – IOC only in polling mode; CHAIN/ENT not needed without
       Link+Event Data TRB (Haiku adds them only with Event Data). */
    uint64_t status = ring_push(&dev->ep0, 0, 0, TRB_TYPE(4) | TRB_IOC |
                                ((!length || !input) ? TRB_DIR_IN : 0));
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    c->doorbell[dev->slot] = 1;
    int result = transfer_wait(dev, status, 0);
    if (result == 0 && length && input && data)
        memcpy(data, dev->ep0_data.virt, length);
    return result;
}

static uint32_t *input_context(struct xhci_device *dev, unsigned index)
{
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    return (uint32_t *)((uint8_t *)dev->input_ctx.virt + (size_t)index * c->context_size);
}

static void fill_slot_context(struct xhci_device *dev, uint8_t entries)
{
    uint32_t *slot = input_context(dev, 1);
    slot[0] = ((uint32_t)dev->speed << 20) | ((uint32_t)entries << 27);
    slot[1] = (uint32_t)dev->port << 16;
}

static void fill_endpoint_context(uint32_t *ep, struct xhci_ring *ring,
                                  uint8_t type, uint16_t max_packet,
                                  uint8_t interval, uint16_t average)
{
    ep[0] = (uint32_t)interval << 16;
    ep[1] = (3u << 1) | ((uint32_t)type << 3) |
            ((uint32_t)max_packet << 16);
    uint64_t dequeue = ring->dma.phys | 1u;
    ep[2] = (uint32_t)dequeue;
    ep[3] = (uint32_t)(dequeue >> 32);
    ep[4] = average;
}

static int reset_port(struct xhci_controller *c, uint8_t port, uint8_t *speed)
{
    volatile uint8_t *portsc = c->op + 0x400 + (uint32_t)(port - 1) * 0x10;
    uint32_t value = rd32(portsc);
    if (!(value & PORTSC_CCS))
        return -1;
    /* Ensure Port Power is on (PP) – real HW needs 20ms after power */
    if (!(value & PORTSC_PP)) {
        uint32_t neutral = value & ~PORTSC_CHANGE_MASK;
        wr32(portsc, neutral | PORTSC_PP);
        xhci_mdelay(20);
        value = rd32(portsc);
        if (!(value & PORTSC_CCS))
            return -1;
    }
    uint32_t before = value;
    uint8_t speed_before = (value >> 10) & 0xf;
    uint32_t neutral = value & ~PORTSC_CHANGE_MASK;
    if (speed_before >= 4) {
        /* USB3.x warm reset */
        wr32(portsc, neutral | PORTSC_WPR);
        if (wait32(portsc, PORTSC_WRC, PORTSC_WRC) < 0) {
            value = rd32(portsc);
            printk("xHCI: port %u warm reset WRC timeout sc=0x%x before=0x%x\n", port, value, before);
            return -1;
        }
        /* Also wait for PED? */
        // wait a bit for link training
        xhci_mdelay(100);
    } else {
        wr32(portsc, neutral | PORTSC_PR);
        if (wait32(portsc, PORTSC_PR, 0) < 0) {
            value = rd32(portsc);
            printk("xHCI: port %u reset PR timeout sc=0x%x before=0x%x\n", port, value, before);
            return -1;
        }
        if (wait32(portsc, PORTSC_PED, PORTSC_PED) < 0) {
            value = rd32(portsc);
            printk("xHCI: port %u reset PED timeout sc=0x%x before=0x%x\n", port, value, before);
            return -1;
        }
    }
    xhci_mdelay(200); /* USB spec: 200ms reset recovery for real HW */
    value = rd32(portsc);
    *speed = (uint8_t)((value >> 10) & 0xf);
    wr32(portsc, value & PORTSC_CHANGE_MASK);
    xhci_mdelay(50);
    printk("xHCI: port %u reset done sc=0x%x speed=%u\n", port, value, *speed);
    return 0;
}

static int disable_slot(struct xhci_controller *c, uint8_t slot)
{
    return command(c, 0, TRB_TYPE(10) | ((uint32_t)slot << 24), 0);
}

static int evaluate_context(struct xhci_device *dev, uint8_t slot)
{
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    return command(c, dev->input_ctx.phys, TRB_TYPE(13) | ((uint32_t)slot << 24), 0);
}

static int stop_endpoint(struct xhci_device *dev, uint8_t ep_num, int is_in)
{
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    uint8_t dci = (uint8_t)(ep_num * 2 + (is_in ? 1 : 0));
    /* Spec encodes EP ID in bits 16-20 for Stop/Reset, slot in 24-31 */
    uint32_t ctrl = TRB_TYPE(15) | ((uint32_t)dev->slot << 24) | ((uint32_t)dci << 16);
    int rc = command(c, 0, ctrl, 0);
    if (rc == -4 || rc == -19) /* Context State / Parameter – already stopped */ 
        return 0;
    return rc;
}

static int reset_endpoint(struct xhci_device *dev, uint8_t ep_num, int is_in)
{
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    uint8_t dci = (uint8_t)(ep_num * 2 + (is_in ? 1 : 0));
    uint32_t ctrl = TRB_TYPE(14) | ((uint32_t)dev->slot << 24) | ((uint32_t)dci << 16);
    return command(c, 0, ctrl, 0);
}

static const char *xhci_cc_string(uint8_t cc)
{
    switch (cc) {
        case 1:  return "Success";
        case 2:  return "Data Buffer Error";
        case 3:  return "Babble";
        case 4:  return "USB Transaction Error";
        case 5:  return "TRB Error";
        case 6:  return "Stall";
        case 7:  return "Resource Error";
        case 8:  return "Bandwidth Error";
        case 9:  return "No Slots";
        case 10: return "Invalid Stream";
        case 11: return "Slot Not Enabled";
        case 12: return "Endpoint Not Enabled";
        case 13: return "Short Packet";
        case 14: return "Ring Underrun";
        case 15: return "Ring Overrun";
        case 17: return "Parameter Error";
        case 18: return "Bandwidth Overrun";
        case 19: return "Context State Error";
        case 22: return "Event Ring Full";
        case 24: return "Halted";
        default: return "Unknown";
    }
}

static int address_device(struct xhci_device *dev)
{
    if (!dev->ctrl) dev->ctrl = cur_hc;
    struct xhci_controller *c = dev->ctrl;
    if (ring_alloc(&dev->ep0) < 0 || dma_alloc(&dev->input_ctx, 1) < 0 ||
        dma_alloc(&dev->device_ctx, 1) < 0 || dma_alloc(&dev->ep0_data, 1) < 0) {
        printk("xHCI: port %u dma/ring alloc failed\n", dev->port);
        return -1;
    }
    uint16_t mps = (dev->speed >= 4) ? 512 : (dev->speed == 3 ? 64 : 8);
    uint8_t slot = 0;
    int rc = command(c, 0, TRB_TYPE(9), &slot);
    if (rc < 0 || slot == 0) {
        printk("xHCI: port %u Enable Slot failed rc=%d slot=%u\n", dev->port, rc, slot);
        return -1;
    }
    dev->slot = slot;
    ((uint64_t *)c->dcbaa.virt)[slot] = dev->device_ctx.phys;
    memset(dev->input_ctx.virt, 0, PAGE_SIZE);
    uint32_t *control = input_context(dev, 0);
    control[1] = 3;
    fill_slot_context(dev, 1);
    fill_endpoint_context(input_context(dev, 2), &dev->ep0, 4, mps, 0, 8);
    barrier();
    printk("xHCI: ADDR port %u slot %u mps %u\n", dev->port, slot, mps);
    rc = command(c, dev->input_ctx.phys, TRB_TYPE(11) |
                         ((uint32_t)slot << 24), 0);
    if (rc < 0) {
        printk("xHCI: port %u Address Device failed rc=%d mps=%u cc=%d\n", dev->port, rc, mps, -rc);
        disable_slot(c, slot);
        dev->slot = 0;
        ((uint64_t *)c->dcbaa.virt)[slot] = 0;
        return rc;
    }
    uint8_t descriptor[8];
    rc = control_transfer(dev, 0x80, 6, 0x0100, 0,
                            descriptor, sizeof(descriptor));
    if (rc < 0) {
        printk("xHCI: port %u GET_DESCRIPTOR(8) failed rc=%d\n", dev->port, rc);
        disable_slot(c, slot);
        dev->slot = 0;
        ((uint64_t *)c->dcbaa.virt)[slot] = 0;
        return rc;
    }
    uint8_t bMaxPacket = descriptor[7];
    uint16_t actual = (dev->speed >= 4) ? (uint16_t)(1u << bMaxPacket) : (uint16_t)bMaxPacket;
    if (actual != mps) {
        printk("xHCI: port %u EP0 MPS %u -> %u from descriptor (raw %u speed %u)\n", dev->port, mps, actual, bMaxPacket, dev->speed);
        if (actual == 0 || actual > 512) {
            printk("xHCI: port %u invalid MPS %u\n", dev->port, actual);
        } else if (actual != mps) {
            /* Evaluate Context to update EP0 MPS without re-addressing.
               Haiku does ConfigureEndpoint/EvaluateContext with input context
               containing new EP0 MaxPacket. */
            memset(dev->input_ctx.virt, 0, PAGE_SIZE);
            uint32_t *ctrl = input_context(dev, 0);
            ctrl[0] = 0;
            ctrl[1] = (1u << 1); /* Add EP0 context (DCI=1) */
            fill_slot_context(dev, 1);
            fill_endpoint_context(input_context(dev, 2), &dev->ep0, 4, actual, 0, 8);
            barrier();
            int erc = evaluate_context(dev, dev->slot);
            if (erc == 0) {
                printk("xHCI: port %u EvaluateContext MPS %u success\n", dev->port, actual);
                mps = actual;
            } else {
                printk("xHCI: port %u EvaluateContext MPS %u failed rc=%d (%s)\n",
                       dev->port, actual, erc, xhci_cc_string((uint8_t)-erc));
                if (actual == 8 || actual == 16 || actual == 32 || actual == 64) {
                    /* Non-critical: keep original mps for now, but log */
                }
            }
        }
    }
    return 0;
}

static int find_boot_interface(struct xhci_device *dev, const uint8_t *cfg,
                               uint16_t length)
{
    uint8_t interface = 0xff;
    uint8_t protocol = 0;
    for (uint16_t off = 0; off + 2 <= length && cfg[off] >= 2;
         off += cfg[off]) {
        if (off + cfg[off] > length)
            break;
        if (cfg[off + 1] == 4 && cfg[off] >= 9) {
            interface = 0xff;
            if (cfg[off + 5] == 3 && cfg[off + 6] == 1 &&
                (cfg[off + 7] == 1 || cfg[off + 7] == 2)) {
                interface = cfg[off + 2];
                protocol = cfg[off + 7];
            }
        } else if (cfg[off + 1] == 5 && cfg[off] >= 7 && interface != 0xff &&
                   (cfg[off + 2] & 0x80) && (cfg[off + 3] & 3) == 3) {
            dev->interface = interface;
            dev->protocol = protocol;
            dev->endpoint = cfg[off + 2] & 0xf;
            dev->max_packet = (uint16_t)(cfg[off + 4] |
                                         ((uint16_t)cfg[off + 5] << 8)) & 0x7ff;
            dev->interval = cfg[off + 6];
            return dev->endpoint && dev->max_packet ? 0 : -1;
        }
    }
    return -1;
}

static int find_mass_storage_interface(struct xhci_device *dev, const uint8_t *cfg,
                                       uint16_t length)
{
    uint8_t interface = 0xff;
    uint8_t bulk_in = 0, bulk_out = 0;
    uint16_t bulk_in_mps = 0, bulk_out_mps = 0;
    for (uint16_t off = 0; off + 2 <= length && cfg[off] >= 2; off += cfg[off]) {
        if (off + cfg[off] > length)
            break;
        if (cfg[off + 1] == 4 && cfg[off] >= 9) {
            uint8_t cls = cfg[off + 5];
            uint8_t sub = cfg[off + 6];
            uint8_t proto = cfg[off + 7];
            if (cls == 0x08 && sub == 0x06 && proto == 0x50) {
                interface = cfg[off + 2];
                bulk_in = 0;
                bulk_out = 0;
                bulk_in_mps = 0;
                bulk_out_mps = 0;
            } else {
                interface = 0xff;
            }
        } else if (cfg[off + 1] == 5 && cfg[off] >= 7 && interface != 0xff) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            if ((attr & 3) == 2) { /* Bulk */
                uint16_t mps = (uint16_t)(cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8)) & 0x7ff;
                if (ep_addr & 0x80) {
                    bulk_in = ep_addr & 0xf;
                    bulk_in_mps = mps;
                } else {
                    bulk_out = ep_addr & 0xf;
                    bulk_out_mps = mps;
                }
                if (bulk_in && bulk_out) {
                    dev->interface = interface;
                    dev->bulk_in_ep = bulk_in;
                    dev->bulk_out_ep = bulk_out;
                    dev->bulk_in_mps = bulk_in_mps ? bulk_in_mps : 512;
                    dev->bulk_out_mps = bulk_out_mps ? bulk_out_mps : 512;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ---- Bulk helpers ---- */
static int xhci_bulk_transfer(struct xhci_device *dev, struct xhci_ring *ring,
                              uint8_t ep_num, int is_in, uint64_t phys,
                              uint32_t len, uint32_t *resid)
{
    if (phys + len > 0xFFFFFFFFULL) {
        printk("xHCI: bulk DMA beyond 32-bit phys 0x%lx len %u\n", (unsigned long)phys, len);
        return -1;
    }
    /* TD_SIZE for Normal TRB: remaining max-packet packets capped 31.
       For bulk single-TRB TD, TD_SIZE=0 is correct; for larger we chunk
       via msc layer so keep 0. */
    uint64_t trb = ring_push(ring, phys, len, TRB_TYPE(1) | TRB_IOC | (is_in ? TRB_DIR_IN : 0));
    uint32_t dci = (uint32_t)ep_num * 2 + (is_in ? 1u : 0u);
    printk("xHCI: bulk trb phys 0x%lx dci %u len %u is_in %d slot %u ep %u\n", (unsigned long)trb, dci, len, is_in, dev->slot, ep_num);
    barrier();
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    c->doorbell[dev->slot] = dci;
    int rc = transfer_wait(dev, trb, resid);
    if (rc < 0) {
        printk("xHCI: bulk transfer failed rc=%d (%s) dci %u len %u\n",
               rc, xhci_cc_string((uint8_t)-rc), dci, len);
        if (rc == -6) {
            /* Stall – Halted endpoint requires Reset (Haiku CancelQueuedTransfers
               does StopEndpoint then ResetEndpoint). */
            printk("xHCI: bulk stall on ep %u is_in %d, resetting endpoint\n", ep_num, is_in);
            int s = stop_endpoint(dev, ep_num, is_in);
            if (s != 0) {
                printk("xHCI: stop endpoint failed %d, retry\n", s);
                s = stop_endpoint(dev, ep_num, is_in);
            }
            int r = reset_endpoint(dev, ep_num, is_in);
            if (r != 0)
                printk("xHCI: reset endpoint failed %d\n", r);
            else
                printk("xHCI: endpoint reset done\n");
            /* Ring dequeue must be reset to current enqueue after stall per spec
               – the enqueue was already advanced by ring_push, so dequeue = phys? */
            uint64_t dequeue = ring->dma.phys | 1u;
            (void)dequeue; /* our ring keeps software state; no HW TR dequeue needed for simple ring */
        }
    }
    return rc;
}

static void msc_delay(void)
{
    xhci_mdelay(20);
}

static int msc_bot(struct xhci_device *dev, const uint8_t *cdb, uint8_t cdb_len,
                   uint32_t xfer_len, int dir_in, void *data)
{
    struct cbw *cbw = (struct cbw *)dev->msc_cbw.virt;
    struct csw *csw = (struct csw *)dev->msc_csw.virt;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIG;
    cbw->tag = ++dev->msc_tag;
    cbw->transfer_length = xfer_len;
    cbw->flags = dir_in ? 0x80 : 0x00;
    cbw->lun = 0;
    cbw->cb_length = cdb_len;
    memcpy(cbw->cb, cdb, cdb_len > 16 ? 16 : cdb_len);
    barrier();

    if (xhci_bulk_transfer(dev, &dev->bulk_out, dev->bulk_out_ep, 0,
                           dev->msc_cbw.phys, 31, 0) < 0)
        return -1;

    if (xfer_len > 0 && data) {
        if (dir_in) {
            memset(dev->msc_data.virt, 0, xfer_len > dev->msc_data.pages * PAGE_SIZE ? dev->msc_data.pages * PAGE_SIZE : xfer_len);
            uint32_t resid = 0;
            int r = xhci_bulk_transfer(dev, &dev->bulk_in, dev->bulk_in_ep, 1,
                                       dev->msc_data.phys, xfer_len, &resid);
            if (r < 0)
                return -1;
            /* Handle short packet: if device sent less than we asked, the
               residual tells us how many bytes were not received. We still
               succeed as long as we got the CSW; short-packet data may be
               legitimate (e.g. last block). The caller must check via CSW. */
            size_t got = xfer_len > resid ? xfer_len - resid : 0;
            if (got > xfer_len) got = xfer_len;
            memcpy(data, dev->msc_data.virt, got);
            if (got < xfer_len)
                memset((uint8_t *)data + got, 0, xfer_len - got);
        } else {
            memcpy(dev->msc_data.virt, data, xfer_len);
            barrier();
            if (xhci_bulk_transfer(dev, &dev->bulk_out, dev->bulk_out_ep, 0,
                                   dev->msc_data.phys, xfer_len, 0) < 0)
                return -1;
        }
    }

    memset(csw, 0, sizeof(*csw));
    barrier();
    uint32_t resid = 0;
    if (xhci_bulk_transfer(dev, &dev->bulk_in, dev->bulk_in_ep, 1,
                           dev->msc_csw.phys, 13, &resid) < 0)
        return -1;
    if (csw->signature != CSW_SIG || csw->tag != cbw->tag)
        return -1;
    if (csw->status != 0)
        return -(int)csw->status - 100;
    return 0;
}

static int msc_inquiry(struct xhci_device *dev)
{
    uint8_t cdb[16] = {0};
    cdb[0] = 0x12;
    cdb[4] = 36;
    uint8_t buf[36];
    memset(buf, 0, sizeof(buf));
    int r = msc_bot(dev, cdb, 6, 36, 1, buf);
    if (r == 0) {
        char vendor[9] = {0}, product[17] = {0};
        memcpy(vendor, buf + 8, 8);
        memcpy(product, buf + 16, 16);
        vendor[8] = 0; product[16] = 0;
        /* Trim trailing spaces */
        for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
        for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
        printk("xHCI: MSC inquiry vendor='%s' product='%s' type=%u\n",
               vendor, product, buf[0] & 0x1f);
    }
    return r;
}

static int msc_test_unit_ready(struct xhci_device *dev)
{
    uint8_t cdb[16] = {0};
    cdb[0] = 0x00;
    for (int i = 0; i < 20; i++) {
        int r = msc_bot(dev, cdb, 6, 0, 0, 0);
        if (r == 0)
            return 0;
        msc_delay();
    }
    return -1;
}

static int msc_read_capacity(struct xhci_device *dev)
{
    uint8_t cdb[16] = {0};
    cdb[0] = 0x25;
    uint8_t buf[8];
    memset(buf, 0, sizeof(buf));
    if (msc_bot(dev, cdb, 10, 8, 1, buf) < 0)
        return -1;
    uint32_t max_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                       ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    uint32_t blk_sz = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                      ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
    if (blk_sz == 0)
        blk_sz = 512;
    dev->block_size = blk_sz;
    dev->block_count = (uint64_t)max_lba + 1;
    printk("xHCI: MSC capacity %lu blocks x %u bytes\n",
           (unsigned long)dev->block_count, blk_sz);
    return 0;
}

static int msc_read_blocks(struct xhci_device *dev, uint64_t lba,
                           uint32_t count, void *buf)
{
    if (count == 0 || !buf)
        return 0;
    uint32_t xfer = count * dev->block_size;
    if (xfer > dev->msc_data.pages * PAGE_SIZE) {
        printk("xHCI: msc_read xfer %u > bounce %u\n", xfer, dev->msc_data.pages*PAGE_SIZE);
        return -1;
    }
    uint8_t cdb[16] = {0};
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count);
    return msc_bot(dev, cdb, 10, xfer, 1, buf);
}

static int msc_write_blocks(struct xhci_device *dev, uint64_t lba,
                            uint32_t count, const void *buf)
{
    if (count == 0 || !buf)
        return 0;
    uint32_t xfer = count * dev->block_size;
    if (xfer > dev->msc_data.pages * PAGE_SIZE) {
        printk("xHCI: msc_write xfer %u > bounce %u\n", xfer, dev->msc_data.pages*PAGE_SIZE);
        return -1;
    }
    uint8_t cdb[16] = {0};
    cdb[0] = 0x2A;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count);
    /* msc_bot expects non-const */
    return msc_bot(dev, cdb, 10, xfer, 0, (void *)buf);
}

/* ---- Block device layer ---- */
static int xhci_block_read(struct block_dev *bd, uint64_t lba, uint32_t count, void *buf)
{
    struct xhci_device *dev = (struct xhci_device *)bd->priv;
    if (!dev || !dev->active || dev->dev_type != DEV_TYPE_MSC)
        return -1;
    if (dev->block_size == 0 || dev->block_count == 0)
        return -1;
    uint8_t *p = (uint8_t *)buf;
    uint64_t total_bytes = (uint64_t)count * 512;
    /* Fast path: native 512-byte blocks — counts are already device blocks. */
    if (dev->block_size == 512) {
        if (lba + count > dev->block_count)
            return -1;
        uint64_t cur = lba;
        uint32_t remain = count;
        while (remain) {
            uint32_t chunk = remain;
            /* Limit per BOT to bounce size (8 pages = 32K = 64*512). */
            uint32_t max_chunk = (dev->msc_data.pages * PAGE_SIZE) / dev->block_size;
            if (chunk > max_chunk) chunk = max_chunk;
            if (msc_read_blocks(dev, cur, chunk, p) < 0)
                return -1;
            cur += chunk;
            p += chunk * 512;
            remain -= chunk;
        }
        return 0;
    }
    /* Non-512 block size: translate 512-sector LBA to device LBA. */
    if (lba * 512 >= dev->block_count * dev->block_size)
        return -1;
    size_t done = 0;
    while (done < total_bytes) {
        uint64_t byte_off = lba * 512 + done;
        uint64_t dev_lba = byte_off / dev->block_size;
        uint32_t dev_off = (uint32_t)(byte_off % dev->block_size);
        /* How many bytes we can copy from this position without crossing a
           device block boundary more than bounce allows. */
        size_t remain = total_bytes - done;
        size_t max_bytes = dev->msc_data.pages * PAGE_SIZE;
        if (remain < max_bytes) max_bytes = remain;
        /* Ensure we don't read a partial device block at the tail unless
           necessary: round max_bytes up to cover the offset inside first block. */
        size_t eff = dev_off + max_bytes;
        uint32_t nblocks = (uint32_t)((eff + dev->block_size - 1) / dev->block_size);
        if (dev_lba + nblocks > dev->block_count)
            nblocks = (uint32_t)(dev->block_count - dev_lba);
        if (nblocks == 0) return -1;
        if (msc_read_blocks(dev, dev_lba, nblocks, dev->msc_data.virt) < 0)
            return -1;
        size_t avail = nblocks * dev->block_size - dev_off;
        size_t copy = remain < avail ? remain : avail;
        memcpy(p + done, (uint8_t *)dev->msc_data.virt + dev_off, copy);
        done += copy;
    }
    return 0;
}

static int xhci_block_write(struct block_dev *bd, uint64_t lba, uint32_t count, const void *buf)
{
    struct xhci_device *dev = (struct xhci_device *)bd->priv;
    if (!dev || !dev->active || dev->dev_type != DEV_TYPE_MSC)
        return -1;
    if (dev->block_size != 512) {
        /* Writes for non-512 blocks require read-modify-write of the
           containing device blocks, not implemented for now. */
        printk("xHCI: write with block_size %u not supported\n", dev->block_size);
        return -1;
    }
    if (dev->block_size == 0) return -1;
    if (lba + count > dev->block_count) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t cur = lba;
    uint32_t remain = count;
    while (remain) {
        uint32_t chunk = remain;
        uint32_t max_chunk = (dev->msc_data.pages * PAGE_SIZE) / dev->block_size;
        if (chunk > max_chunk) chunk = max_chunk;
        /* Copy into bounce before issuing BOT OUT */
        memcpy(dev->msc_data.virt, p, chunk * dev->block_size);
        if (msc_write_blocks(dev, cur, chunk, dev->msc_data.virt) < 0)
            return -1;
        cur += chunk;
        p += chunk * 512;
        remain -= chunk;
    }
    return 0;
}

/* Device ops for /dev access (similar to sata/nvme). */
static long xhci_dev_read(struct device *d, uint64_t off, void *buf, size_t len)
{
    struct xhci_device *dev = (struct xhci_device *)d->priv;
    if (!dev || !dev->active || dev->dev_type != DEV_TYPE_MSC)
        return -1;
    if (dev->block_size == 0) return -1;
    uint64_t lba = off / dev->block_size;
    uint32_t nblocks = (uint32_t)((len + dev->block_size - 1) / dev->block_size);
    if (nblocks == 0) return 0;
    uint8_t *tmp = (uint8_t *)kmalloc(nblocks * dev->block_size);
    if (!tmp) return -1;
    if (msc_read_blocks(dev, lba, nblocks, tmp) < 0) { kfree(tmp); return -1; }
    size_t copy = len < nblocks * dev->block_size ? len : nblocks * dev->block_size;
    memcpy(buf, tmp, copy);
    kfree(tmp);
    return (long)copy;
}

static long xhci_dev_write(struct device *d, uint64_t off, const void *buf, size_t len)
{
    struct xhci_device *dev = (struct xhci_device *)d->priv;
    if (!dev || !dev->active || dev->dev_type != DEV_TYPE_MSC)
        return -1;
    if (dev->block_size != 512) return -1;
    uint64_t lba = off / dev->block_size;
    uint32_t nblocks = (uint32_t)((len + dev->block_size - 1) / dev->block_size);
    if (nblocks == 0) return 0;
    size_t alloc = nblocks * dev->block_size;
    uint8_t *tmp = (uint8_t *)kmalloc(alloc);
    if (!tmp) return -1;
    memcpy(tmp, buf, len);
    if (len < alloc) memset(tmp + len, 0, alloc - len);
    int r = msc_write_blocks(dev, lba, nblocks, tmp);
    kfree(tmp);
    if (r < 0) return -1;
    return (long)len;
}

static int configure_msc(struct xhci_device *dev)
{
    uint8_t device_desc[18];
    if (control_transfer(dev, 0x80, 6, 0x0100, 0, device_desc, sizeof(device_desc)) < 0)
        return -1;
    uint8_t config_head[9];
    if (control_transfer(dev, 0x80, 6, 0x0200, 0, config_head, sizeof(config_head)) < 0)
        return -1;
    uint16_t total = (uint16_t)(config_head[2] | ((uint16_t)config_head[3] << 8));
    if (total < 9 || total > 512)
        return -1;
    uint8_t config[512];
    if (control_transfer(dev, 0x80, 6, 0x0200, 0, config, total) < 0)
        return -1;
    if (find_mass_storage_interface(dev, config, total) < 0)
        return -1;
    dev->configuration = config[5];
    if (ring_alloc(&dev->bulk_in) < 0 || ring_alloc(&dev->bulk_out) < 0)
        return -1;
    if (dma_alloc(&dev->msc_cbw, 1) < 0 || dma_alloc(&dev->msc_csw, 1) < 0 ||
        dma_alloc(&dev->msc_data, 8) < 0)
        return -1;

    memset(dev->input_ctx.virt, 0, PAGE_SIZE);
    uint8_t dci_in = (uint8_t)(dev->bulk_in_ep * 2 + 1);
    uint8_t dci_out = (uint8_t)(dev->bulk_out_ep * 2);
    uint8_t max_dci = dci_in > dci_out ? dci_in : dci_out;
    uint32_t *ctrl = input_context(dev, 0);
    ctrl[1] = 1u | (1u << dci_in) | (1u << dci_out);
    fill_slot_context(dev, max_dci);
    /* xHCI EP types: 2 = Bulk OUT, 6 = Bulk IN */
    fill_endpoint_context(input_context(dev, dci_in + 1), &dev->bulk_in, 6,
                          dev->bulk_in_mps, 0, dev->bulk_in_mps);
    fill_endpoint_context(input_context(dev, dci_out + 1), &dev->bulk_out, 2,
                          dev->bulk_out_mps, 0, dev->bulk_out_mps);
    if (command(dev->ctrl, dev->input_ctx.phys, TRB_TYPE(12) | ((uint32_t)dev->slot << 24), 0) < 0)
        return -1;
    if (control_transfer(dev, 0x00, 9, dev->configuration, 0, 0, 0) < 0)
        return -1;

    /* Bulk-Only Transport: reset (class request) is optional; we skip it
       and go straight to SCSI initialization. Some devices require it, but
       the BOT reset is best-effort and failure is ignored. */
    control_transfer(dev, 0x21, 0xFF, 0, dev->interface, 0, 0);

    dev->msc_tag = 0;
    if (msc_inquiry(dev) < 0)
        printk("xHCI: MSC inquiry failed (port %u)\n", dev->port);
    if (msc_test_unit_ready(dev) < 0) {
        printk("xHCI: MSC not ready after retries (port %u)\n", dev->port);
        return -1;
    }
    if (msc_read_capacity(dev) < 0)
        return -1;

    /* Register block device for VFS + /dev */
    dev->blk.bus = 99; /* marker for USB */
    dev->blk.drive = dev->port;
    dev->blk.present = 1;
    dev->blk.total_sectors = dev->block_count * (dev->block_size / 512);
    if (dev->block_size % 512 != 0) {
        /* Round up sector count for non-512. */
        dev->blk.total_sectors = (dev->block_count * dev->block_size + 511) / 512;
    }
    dev->blk.read_fn = xhci_block_read;
    dev->blk.write_fn = xhci_block_write;
    dev->blk.priv = dev;

    if (!device_find("usb0")) {
        struct device *d = (struct device *)kmalloc(sizeof(struct device));
        if (d) {
            memset(d, 0, sizeof(*d));
            strcpy(d->name, "usb0");
            d->major = 0x55; d->minor = 0;
            d->type = DEV_BLOCK;
            d->priv = dev;
            d->ops.read = xhci_dev_read;
            d->ops.write = xhci_dev_write;
            device_register(d);
            dev->blk_dev = d;
        }
    } else {
        /* Additional LUNs get usb1, usb2... */
        for (int i = 1; i < XHCI_MAX_PORTS; i++) {
            char name[16];
            name[0]='u'; name[1]='s'; name[2]='b';
            if (i < 10) { name[3]=(char)('0'+i); name[4]=0; }
            else { name[3]=(char)('0'+i/10); name[4]=(char)('0'+i%10); name[5]=0; }
            if (!device_find(name)) {
                struct device *d = (struct device *)kmalloc(sizeof(struct device));
                if (!d) break;
                memset(d, 0, sizeof(*d));
                strcpy(d->name, name);
                d->major = 0x55; d->minor = (uint32_t)i;
                d->type = DEV_BLOCK;
                d->priv = dev;
                d->ops.read = xhci_dev_read;
                d->ops.write = xhci_dev_write;
                device_register(d);
                dev->blk_dev = d;
                break;
            }
        }
    }

    dev->dev_type = DEV_TYPE_MSC;
    dev->active = 1;
    printk("xHCI: port %u slot %u MSC bulk IN %u(%u) OUT %u(%u) %lu x %u -> /dev/%s\n",
           dev->port, dev->slot, dev->bulk_in_ep, dev->bulk_in_mps,
           dev->bulk_out_ep, dev->bulk_out_mps,
           (unsigned long)dev->block_count, dev->block_size,
           dev->blk_dev ? dev->blk_dev->name : "usb?");
    return 0;
}

static int configure_hid(struct xhci_device *dev)
{
    uint8_t device_desc[18];
    if (control_transfer(dev, 0x80, 6, 0x0100, 0,
                         device_desc, sizeof(device_desc)) < 0)
        return -1;
    uint8_t config_head[9];
    if (control_transfer(dev, 0x80, 6, 0x0200, 0,
                         config_head, sizeof(config_head)) < 0)
        return -1;
    uint16_t total = (uint16_t)(config_head[2] | ((uint16_t)config_head[3] << 8));
    if (total < 9 || total > 512)
        return -1;
    uint8_t config[512];
    if (control_transfer(dev, 0x80, 6, 0x0200, 0, config, total) < 0 ||
        find_boot_interface(dev, config, total) < 0)
        return -1;
    dev->configuration = config[5];
    if (ring_alloc(&dev->intr) < 0 || dma_alloc(&dev->report, 1) < 0)
        return -1;

    memset(dev->input_ctx.virt, 0, PAGE_SIZE);
    uint8_t dci = (uint8_t)(dev->endpoint * 2 + 1);
    uint32_t *control = input_context(dev, 0);
    control[1] = 1u | (1u << dci);
    fill_slot_context(dev, dci);
    uint8_t interval = dev->speed <= 2 ? (uint8_t)(dev->interval + 2) :
                                        (uint8_t)(dev->interval - 1);
    fill_endpoint_context(input_context(dev, dci + 1), &dev->intr, 7,
                          dev->max_packet, interval, dev->max_packet);
    if (command(dev->ctrl, dev->input_ctx.phys, TRB_TYPE(12) |
                ((uint32_t)dev->slot << 24), 0) < 0)
        return -1;
    if (control_transfer(dev, 0x00, 9, dev->configuration, 0, 0, 0) < 0)
        return -1;
    if (control_transfer(dev, 0x21, 11, 0, dev->interface, 0, 0) < 0)
        return -1;
    dev->report_size = dev->protocol == 1 ? 8 : 3;
    dev->dev_type = DEV_TYPE_HID;
    dev->active = 1;
    printk("xHCI: port %u slot %u HID boot %s ep %u mps %u\n",
           dev->port, dev->slot, dev->protocol == 1 ? "keyboard" : "mouse",
           dev->endpoint, dev->max_packet);
    return 0;
}

static void queue_interrupt(struct xhci_device *dev)
{
    if (dev->dev_type != DEV_TYPE_HID) return;
    if (dev->report.phys + dev->report_size > 0xFFFFFFFFULL) {
        printk("xHCI: HID report DMA beyond 32-bit\n");
        return;
    }
    memset(dev->report.virt, 0, dev->report_size);
    dev->pending_trb = ring_push(&dev->intr, dev->report.phys,
                                 dev->report_size, TRB_TYPE(1) | TRB_IOC);
    dev->pending = 1;
    barrier();
    struct xhci_controller *c = dev->ctrl ? dev->ctrl : cur_hc;
    c->doorbell[dev->slot] = (uint32_t)(dev->endpoint * 2 + 1);
}

static void handle_transfer_event(struct xhci_controller *c, volatile struct xhci_trb *event)
{
    uint8_t slot = (uint8_t)(event->control >> 24);
    uint64_t pointer = event->parameter & ~0xfULL;
    uint8_t completion = (uint8_t)(event->status >> 24);
    uint32_t residual = event->status & 0xffffffu;
    for (uint8_t i = 0; i < c->max_ports; i++) {
        struct xhci_device *dev = &c->devices[i];
        if (!dev->active || dev->dev_type != DEV_TYPE_HID || !dev->pending || dev->slot != slot ||
            dev->pending_trb != pointer)
            continue;
        dev->pending = 0;
        if (completion == 1 || completion == 13) {
            uint32_t length = residual < dev->report_size ?
                              dev->report_size - residual : 0;
            uint8_t *report = (uint8_t *)dev->report.virt;
            if (dev->protocol == 1 && length >= 8)
                keyboard_hid_boot_report(report[0], &report[2]);
            else if (dev->protocol == 2 && length >= 3)
                mouse_submit_event((int)(int8_t)report[1],
                                   (int)(int8_t)report[2], report[0] & 7);
        }
        queue_interrupt(dev);
        return;
    }
}

static void handle_hid_event_inline(volatile struct xhci_trb *event)
{
    /* Same HID dispatch – try all controllers */
    uint8_t slot = (uint8_t)(event->control >> 24);
    uint64_t pointer = event->parameter & ~0xfULL;
    uint8_t completion = (uint8_t)(event->status >> 24);
    uint32_t residual = event->status & 0xffffffu;
    for (int ci = 0; ci < n_hcs; ci++) {
        struct xhci_controller *c = &hcs[ci];
        for (uint8_t i = 0; i < c->max_ports; i++) {
            struct xhci_device *dev = &c->devices[i];
            if (!dev->active || dev->dev_type != DEV_TYPE_HID || !dev->pending ||
                dev->slot != slot || dev->pending_trb != pointer)
                continue;
            dev->pending = 0;
            if (completion == 1 || completion == 13) {
                uint32_t length = residual < dev->report_size ?
                                  dev->report_size - residual : 0;
                uint8_t *report = (uint8_t *)dev->report.virt;
                if (dev->protocol == 1 && length >= 8)
                    keyboard_hid_boot_report(report[0], &report[2]);
                else if (dev->protocol == 2 && length >= 3)
                    mouse_submit_event((int)(int8_t)report[1],
                                       (int)(int8_t)report[2], report[0] & 7);
            }
            queue_interrupt(dev);
            return;
        }
    }
}

void xhci_poll(void)
{
    for (int ci = 0; ci < n_hcs; ci++) {
        struct xhci_controller *c = &hcs[ci];
        if (!c->ready) continue;
        cur_hc = c;
        int had_event = 0;
        for (;;) {
            volatile struct xhci_trb *event = event_current(c);
            if (!event)
                break;
            had_event = 1;
            uint32_t type = (event->control >> 10) & 0x3f;
            if (type == 32)
                handle_transfer_event(c, event);
            else if (type == 34) {
                /* Port Status Change – just ack, enumeration already polls CSC */
            }
            event_advance(c);
        }
        if (had_event && c->irq_enabled) {
            xhci_ack_irq(c);
            c->irq_pending = 0;
        }
        for (uint8_t i = 0; i < c->max_ports; i++)
            if (c->devices[i].active && c->devices[i].dev_type == DEV_TYPE_HID && !c->devices[i].pending)
                queue_interrupt(&c->devices[i]);
    }
    cur_hc = NULL;
}

static int controller_start(void)
{
    uint32_t cap0 = rd32(hc.mmio);
    hc.cap_length = (uint8_t)cap0;
    hc.op = hc.mmio + hc.cap_length;
    uint32_t hcs1 = rd32(hc.mmio + 4);
    uint32_t hcs2 = rd32(hc.mmio + 8);
    uint32_t hcc = rd32(hc.mmio + 0x10);
    hc.max_slots = (uint8_t)(hcs1 & 0xff);
    hc.max_ports = (uint8_t)((hcs1 >> 24) & 0xff);
    if (hc.max_ports > XHCI_MAX_PORTS)
        hc.max_ports = XHCI_MAX_PORTS;
    hc.context_size = (hcc & (1u << 2)) ? 64 : 32;
    hc.doorbell = (volatile uint32_t *)(hc.mmio + (rd32(hc.mmio + 0x14) & ~3u));
    hc.runtime = hc.mmio + (rd32(hc.mmio + 0x18) & ~0x1fu);
    printk("xHCI: cap hcc=0x%x AC64=%u CSZ=%u\n", hcc, hcc & 1u, hc.context_size);
    legacy_handoff(cur_hc, hcc);

    wr32(hc.op, rd32(hc.op) & ~1u);
    xhci_mdelay(10);
    if (wait32(hc.op + 4, 1u, 1u) < 0) {
        printk("xHCI: halt timeout sts=0x%x\n", rd32(hc.op+4));
        return -1;
    }
    wr32(hc.op, rd32(hc.op) | 2u);
    xhci_mdelay(10);
    if (wait32(hc.op, 2u, 0) < 0 || wait32(hc.op + 4, 1u << 11, 0) < 0) {
        printk("xHCI: reset timeout cmd=0x%x sts=0x%x\n", rd32(hc.op), rd32(hc.op+4));
        return -1;
    }
    xhci_mdelay(10);
    if (!(rd32(hc.op + 8) & 1u)) {
        printk("xHCI: controller not ready after reset\n");
        return -1;
    }

    if (dma_alloc(&hc.dcbaa, 1) < 0 || dma_alloc(&hc.command, 1) < 0 ||
        dma_alloc(&hc.event, 1) < 0 || dma_alloc(&hc.erst, 1) < 0)
        return -1;
    hc.cmd_cycle = 1;
    hc.event_cycle = 1;
    uint32_t scratch_count = ((hcs2 >> 27) & 0x1f) << 5 |
                             ((hcs2 >> 21) & 0x1f);
    if (scratch_count > 32)
        scratch_count = 32;
    if (scratch_count) {
        if (dma_alloc(&hc.scratch_array, 1) < 0)
            return -1;
        for (uint32_t i = 0; i < scratch_count; i++) {
            if (dma_alloc(&hc.scratch[i], 1) < 0)
                return -1;
            ((uint64_t *)hc.scratch_array.virt)[i] = hc.scratch[i].phys;
        }
        ((uint64_t *)hc.dcbaa.virt)[0] = hc.scratch_array.phys;
    }
    volatile struct xhci_trb *cmd =
        (volatile struct xhci_trb *)hc.command.virt;
    cmd[XHCI_RING_TRBS - 1].parameter = hc.command.phys;
    cmd[XHCI_RING_TRBS - 1].control = TRB_TYPE(6) | TRB_TC | 1;

    uint64_t *erst = (uint64_t *)hc.erst.virt;
    erst[0] = hc.event.phys;
    ((uint32_t *)erst)[2] = XHCI_RING_TRBS;
    volatile uint8_t *ir = hc.runtime + 0x20;
    wr32(ir + 8, 1);
    wr64(ir + 0x10, hc.erst.phys);
    wr64(ir + 0x18, hc.event.phys);
    wr64(hc.op + 0x30, hc.dcbaa.phys);
    wr64(hc.op + 0x18, hc.command.phys | 1u);
    wr32(hc.op + 0x38, hc.max_slots);
    barrier();
    wr32(hc.op, 1u);
    if (wait32(hc.op + 4, 1u, 0) < 0)
        return -1;
    printk("xHCI: running slots=%u ports=%u contexts=%u scratchpads=%u\n",
           hc.max_slots, hc.max_ports, hc.context_size, scratch_count);
    return 0;
}

/* Public helpers for VFS */
struct block_dev *xhci_get_block_dev(int idx)
{
    int seen = 0;
    for (int ci = 0; ci < n_hcs; ci++) {
        struct xhci_controller *c = &hcs[ci];
        for (int i = 0; i < c->max_ports; i++) {
            struct xhci_device *d = &c->devices[i];
            if (d->active && d->dev_type == DEV_TYPE_MSC) {
                if (seen == idx) return &d->blk;
                seen++;
            }
        }
    }
    return 0;
}

int xhci_block_count(void)
{
    int n = 0;
    for (int ci = 0; ci < n_hcs; ci++) {
        struct xhci_controller *c = &hcs[ci];
        for (int i = 0; i < c->max_ports; i++)
            if (c->devices[i].active && c->devices[i].dev_type == DEV_TYPE_MSC) n++;
    }
    return n;
}

int xhci_storage_read(uint64_t lba, uint32_t count, void *buf)
{
    struct block_dev *bd = xhci_get_block_dev(0);
    if (!bd) return -1;
    return blk_read(bd, lba, count, buf);
}

static void xhci_switch_ports(struct pci_device *pdev)
{
    /* Intel quirk only – AMD 1022:* does not use XUSB2PR (Intel 8086 specific).
       Haiku xhci.cpp _SwitchIntelPorts() checks vendor == Intel and device
       == 0x1E31/0x9C31 etc. Do not touch AMD. */
    if (pdev->vendor != 0x8086)
        return;
    uint32_t v;
    v = pci_read32(pdev->bus, pdev->dev, pdev->func, 0xD0);
    printk("xHCI: XUSB2PR before 0x%x\n", v);
    pci_write32(pdev->bus, pdev->dev, pdev->func, 0xD0, 0xFFFFFFFF);
    v = pci_read32(pdev->bus, pdev->dev, pdev->func, 0xD0);
    printk("xHCI: XUSB2PR after 0x%x\n", v);
    v = pci_read32(pdev->bus, pdev->dev, pdev->func, 0xD8);
    printk("xHCI: USB3_PSSEN before 0x%x\n", v);
    pci_write32(pdev->bus, pdev->dev, pdev->func, 0xD8, 0xFFFFFFFF);
    v = pci_read32(pdev->bus, pdev->dev, pdev->func, 0xD8);
    printk("xHCI: USB3_PSSEN after 0x%x\n", v);
}

int xhci_probe(struct pci_device *pdev)
{
    if (pdev->class_code != 0x0c || pdev->subclass != 0x03 ||
        pdev->prog_if != 0x30)
        return -1;
    if (n_hcs >= XHCI_MAX_CONTROLLERS)
        return -1;
    /* Avoid double-probing same bar */
    uint64_t bar = pci_bar_addr(pdev, 0);
    if (!bar || (pdev->bar[0] & 1))
        return -1;
    for (int i = 0; i < n_hcs; i++) {
        if (hcs[i].mmio && hcs[i].mmio == (volatile uint8_t *)(uintptr_t)bar)
            return -1;
    }
    cur_hc = &hcs[n_hcs];
    memset(cur_hc, 0, sizeof(*cur_hc));
    /* Port switching quirk – must be before HC init (Linux 69e848c) */
    xhci_switch_ports(pdev);
    uint32_t command_reg = pci_read32(pdev->bus, pdev->dev, pdev->func, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->func, 0x04,
                (command_reg & 0xFFFFu) | (1u << 1) | (1u << 2));
    cur_hc->mmio = (volatile uint8_t *)vmm_mmap_phys(bar, 16,
                                                MMU_WRITE | MMU_UNCACHED);
    if (!cur_hc->mmio || controller_start() < 0) {
        printk("xHCI: controller %d initialization failed\n", n_hcs);
        cur_hc->mmio = 0;
        cur_hc = NULL;
        return -1;
    }
    /* Try to enable interrupt mode (polling remains fallback). */
    (void)xhci_setup_irq(cur_hc, pdev);
    printk("xHCI: controller %d at bar 0x%lx slots=%u ports=%u irq %s (vector 0x%x gsi %u)\n",
           n_hcs, bar, cur_hc->max_slots, cur_hc->max_ports,
           cur_hc->irq_enabled ? "enabled" : "polling",
           cur_hc->irq_vector, cur_hc->irq_gsi);
    for (uint8_t port = 1; port <= cur_hc->max_ports; port++) {
        struct xhci_device *dev = &cur_hc->devices[port - 1];
        dev->port = port;
        dev->ctrl = cur_hc;
        if (reset_port(cur_hc, port, &dev->speed) < 0)
            continue;
        printk("xHCI: port %u speed %u\n", port, dev->speed);
        int addr_ok = 0;
        for (int tries = 0; tries < 2; tries++) {
            if (address_device(dev) == 0) { addr_ok = 1; break; }
            if (tries == 0)
                printk("xHCI: port %u address retry\n", port);
            xhci_mdelay(100);
            if (reset_port(cur_hc, port, &dev->speed) < 0) break;
        }
        if (!addr_ok) {
            printk("xHCI: port %u address failed\n", port);
            continue;
        }
        /* Determine class before Configure Endpoint – do not try HID then MSC sequentially */
        uint8_t dev_desc_try[18];
        uint8_t cfg_head_try[9];
        int is_hid = 0, is_msc = 0, is_hub = 0;
        if (control_transfer(dev, 0x80, 6, 0x0100, 0, dev_desc_try, 18) == 0 &&
            control_transfer(dev, 0x80, 6, 0x0200, 0, cfg_head_try, 9) == 0) {
            uint16_t tot = cfg_head_try[2] | ((uint16_t)cfg_head_try[3] << 8);
            if (tot >= 9 && tot <= 512) {
                uint8_t cfg_try[512];
                if (control_transfer(dev, 0x80, 6, 0x0200, 0, cfg_try, tot) == 0) {
                    for (uint16_t off = 0; off + 9 <= tot && cfg_try[off] >= 2; off += cfg_try[off]) {
                        if (cfg_try[off+1] != 4 || cfg_try[off] < 9) continue;
                        uint8_t cls = cfg_try[off+5], sub = cfg_try[off+6], proto = cfg_try[off+7];
                        if (cls == 3 && sub == 1 && (proto == 1 || proto == 2)) is_hid = 1;
                        if (cls == 8 && sub == 6 && proto == 0x50) is_msc = 1;
                        if (cls == 9) is_hub = 1;
                    }
                }
            }
        }
        if (is_hid && !is_msc) {
            if (configure_hid(dev) == 0) { queue_interrupt(dev); continue; }
            printk("xHCI: controller %d port %u HID configure failed\n", n_hcs, port);
        } else if (is_msc) {
            if (configure_msc(dev) == 0) continue;
            printk("xHCI: controller %d port %u MSC configure failed\n", n_hcs, port);
        } else if (is_hub) {
            printk("xHCI: controller %d port %u is hub (not yet supported)\n", n_hcs, port);
        } else {
            printk("xHCI: controller %d port %u unsupported class (not HID/MSC/hub)\n", n_hcs, port);
        }
    }
    cur_hc->ready = 1;
    n_hcs++;
    cur_hc = NULL;
    return 0;
}
