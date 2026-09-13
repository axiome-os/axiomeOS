#include "xhci.h"

#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "pmm.h"
#include "printk.h"
#include "string.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

#define XHCI_MAX_PORTS 16
#define XHCI_RING_TRBS 256
#define XHCI_TIMEOUT 4000000u

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

struct xhci_device {
    int active;
    int pending;
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    uint8_t protocol;
    uint8_t configuration;
    uint8_t interface;
    uint8_t endpoint;
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
};

static struct xhci_controller hc;

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

static int wait32(volatile void *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT; i++)
        if ((rd32(reg) & mask) == value)
            return 0;
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
        link->control = TRB_TYPE(6) | TRB_ENT | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    barrier();
    return ring->dma.phys + (uint64_t)index * sizeof(struct xhci_trb);
}

static void legacy_handoff(uint32_t hcc)
{
    uint32_t off = ((hcc >> 16) & 0xffffu) * 4u;
    while (off) {
        volatile uint8_t *ext = hc.mmio + off;
        uint32_t cap = rd32(ext);
        uint8_t id = (uint8_t)cap;
        uint8_t next = (uint8_t)(cap >> 8);
        if (id == 1) {
            wr32(ext, cap | (1u << 24));
            for (uint32_t i = 0; i < XHCI_TIMEOUT; i++) {
                cap = rd32(ext);
                if (!(cap & (1u << 16)))
                    break;
            }
            wr32(ext + 4, 0);
            printk("xHCI: legacy ownership acquired\n");
            return;
        }
        if (!next)
            break;
        off += (uint32_t)next * 4u;
    }
}

static volatile struct xhci_trb *event_current(void)
{
    volatile struct xhci_trb *event =
        &((volatile struct xhci_trb *)hc.event.virt)[hc.event_dequeue];
    if ((event->control & TRB_CYCLE) != hc.event_cycle)
        return 0;
    return event;
}

static void event_advance(void)
{
    hc.event_dequeue++;
    if (hc.event_dequeue == XHCI_RING_TRBS) {
        hc.event_dequeue = 0;
        hc.event_cycle ^= 1;
    }
    volatile uint8_t *ir = hc.runtime + 0x20;
    wr64(ir + 0x18, (hc.event.phys +
         (uint64_t)hc.event_dequeue * sizeof(struct xhci_trb)) | (1u << 3));
}

static int command(uint64_t parameter, uint32_t control, uint8_t *slot_out)
{
    uint16_t index = hc.cmd_enqueue;
    volatile struct xhci_trb *trb =
        &((volatile struct xhci_trb *)hc.command.virt)[index];
    uint64_t command_phys = hc.command.phys +
                            (uint64_t)index * sizeof(struct xhci_trb);
    trb->parameter = parameter;
    trb->status = 0;
    barrier();
    trb->control = control | hc.cmd_cycle;
    hc.cmd_enqueue++;
    if (hc.cmd_enqueue == XHCI_RING_TRBS - 1) {
        volatile struct xhci_trb *link =
            &((volatile struct xhci_trb *)hc.command.virt)[hc.cmd_enqueue];
        link->parameter = hc.command.phys;
        link->status = 0;
        barrier();
        link->control = TRB_TYPE(6) | TRB_ENT | hc.cmd_cycle;
        hc.cmd_enqueue = 0;
        hc.cmd_cycle ^= 1;
    }
    barrier();
    hc.doorbell[0] = 0;

    for (uint32_t guard = 0; guard < XHCI_TIMEOUT; guard++) {
        volatile struct xhci_trb *event = event_current();
        if (!event)
            continue;
        uint32_t type = (event->control >> 10) & 0x3f;
        uint64_t pointer = event->parameter & ~0xfULL;
        uint8_t completion = (uint8_t)(event->status >> 24);
        uint8_t slot = (uint8_t)(event->control >> 24);
        event_advance();
        if (type == 33 && pointer == command_phys) {
            if (slot_out)
                *slot_out = slot;
            return completion == 1 ? 0 : -(int)completion;
        }
    }
    return -255;
}

static int transfer_wait(struct xhci_device *dev, uint64_t wanted,
                         uint32_t *residual)
{
    for (uint32_t guard = 0; guard < XHCI_TIMEOUT; guard++) {
        volatile struct xhci_trb *event = event_current();
        if (!event)
            continue;
        uint32_t type = (event->control >> 10) & 0x3f;
        uint64_t pointer = event->parameter & ~0xfULL;
        uint8_t completion = (uint8_t)(event->status >> 24);
        uint8_t slot = (uint8_t)(event->control >> 24);
        uint32_t remain = event->status & 0xffffffu;
        event_advance();
        if (type == 32 && slot == dev->slot && pointer == wanted) {
            if (residual)
                *residual = remain;
            return (completion == 1 || completion == 13) ? 0 : -(int)completion;
        }
    }
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
    ring_push(&dev->ep0, setup_value(request_type, request, value, index, length),
              8, TRB_TYPE(2) | (1u << 6) | TRB_CHAIN | (trt << 16));
    if (length) {
        if (!input && data)
            memcpy(dev->ep0_data.virt, data, length);
        ring_push(&dev->ep0, dev->ep0_data.phys, length,
                  TRB_TYPE(3) | (input ? TRB_DIR_IN : 0u) | TRB_CHAIN);
    }
    uint64_t status = ring_push(&dev->ep0, 0, 0, TRB_TYPE(4) | TRB_IOC |
                                ((!length || !input) ? TRB_DIR_IN : 0));
    hc.doorbell[dev->slot] = 1;
    int result = transfer_wait(dev, status, 0);
    if (result == 0 && length && input && data)
        memcpy(data, dev->ep0_data.virt, length);
    return result;
}

static uint32_t *input_context(struct xhci_device *dev, unsigned index)
{
    return (uint32_t *)((uint8_t *)dev->input_ctx.virt +
                        (size_t)index * hc.context_size);
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

static int reset_port(uint8_t port, uint8_t *speed)
{
    volatile uint8_t *portsc = hc.op + 0x400 + (uint32_t)(port - 1) * 0x10;
    uint32_t value = rd32(portsc);
    if (!(value & 1u))
        return -1;
    uint32_t change = (1u << 17) | (1u << 18) | (1u << 20) |
                      (1u << 21) | (1u << 22) | (1u << 23);
    wr32(portsc, (value & ~change) | (1u << 4));
    if (wait32(portsc, 1u << 4, 0) < 0 || wait32(portsc, 1u << 1, 1u << 1) < 0)
        return -1;
    value = rd32(portsc);
    *speed = (uint8_t)((value >> 10) & 0xf);
    wr32(portsc, value & change);
    return 0;
}

static int address_device(struct xhci_device *dev)
{
    if (ring_alloc(&dev->ep0) < 0 || dma_alloc(&dev->input_ctx, 1) < 0 ||
        dma_alloc(&dev->device_ctx, 1) < 0 || dma_alloc(&dev->ep0_data, 1) < 0)
        return -1;
    uint8_t slot = 0;
    if (command(0, TRB_TYPE(9), &slot) < 0 || slot == 0)
        return -1;
    dev->slot = slot;
    ((uint64_t *)hc.dcbaa.virt)[slot] = dev->device_ctx.phys;
    uint32_t *control = input_context(dev, 0);
    control[1] = 3;
    fill_slot_context(dev, 1);
    uint16_t mps = dev->speed == 4 ? 512 : (dev->speed == 3 ? 64 : 8);
    fill_endpoint_context(input_context(dev, 2), &dev->ep0, 4, mps, 0, 8);
    barrier();
    int result = command(dev->input_ctx.phys, TRB_TYPE(11) |
                         ((uint32_t)slot << 24), 0);
    if (result < 0)
        return result;
    uint8_t descriptor[8];
    return control_transfer(dev, 0x80, 6, 0x0100, 0,
                            descriptor, sizeof(descriptor));
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

static int configure_device(struct xhci_device *dev)
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
    if (command(dev->input_ctx.phys, TRB_TYPE(12) |
                ((uint32_t)dev->slot << 24), 0) < 0)
        return -1;
    if (control_transfer(dev, 0x00, 9, dev->configuration, 0, 0, 0) < 0)
        return -1;
    if (control_transfer(dev, 0x21, 11, 0, dev->interface, 0, 0) < 0)
        return -1;
    dev->report_size = dev->protocol == 1 ? 8 : 3;
    dev->active = 1;
    printk("xHCI: port %u slot %u HID boot %s ep %u mps %u\n",
           dev->port, dev->slot, dev->protocol == 1 ? "keyboard" : "mouse",
           dev->endpoint, dev->max_packet);
    return 0;
}

static void queue_interrupt(struct xhci_device *dev)
{
    memset(dev->report.virt, 0, dev->report_size);
    dev->pending_trb = ring_push(&dev->intr, dev->report.phys,
                                 dev->report_size, TRB_TYPE(1) | TRB_IOC);
    dev->pending = 1;
    barrier();
    hc.doorbell[dev->slot] = (uint32_t)(dev->endpoint * 2 + 1);
}

static void handle_transfer_event(volatile struct xhci_trb *event)
{
    uint8_t slot = (uint8_t)(event->control >> 24);
    uint64_t pointer = event->parameter & ~0xfULL;
    uint8_t completion = (uint8_t)(event->status >> 24);
    uint32_t residual = event->status & 0xffffffu;
    for (uint8_t i = 0; i < hc.max_ports; i++) {
        struct xhci_device *dev = &hc.devices[i];
        if (!dev->active || !dev->pending || dev->slot != slot ||
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
                /* USB HID boot-protocol Y is already screen-oriented (+down,
                   opposite of PS/2's Cartesian +up): pass it through as-is
                   so the queue stays +right/+down like the PS/2 path. */
                mouse_submit_event((int)(int8_t)report[1],
                                   (int)(int8_t)report[2], report[0] & 7);
        }
        queue_interrupt(dev);
        return;
    }
}

void xhci_poll(void)
{
    if (!hc.ready)
        return;
    for (;;) {
        volatile struct xhci_trb *event = event_current();
        if (!event)
            break;
        uint32_t type = (event->control >> 10) & 0x3f;
        if (type == 32)
            handle_transfer_event(event);
        event_advance();
    }
    for (uint8_t i = 0; i < hc.max_ports; i++)
        if (hc.devices[i].active && !hc.devices[i].pending)
            queue_interrupt(&hc.devices[i]);
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
    legacy_handoff(hcc);

    wr32(hc.op, rd32(hc.op) & ~1u);
    if (wait32(hc.op + 4, 1u, 1u) < 0)
        return -1;
    wr32(hc.op, rd32(hc.op) | 2u);
    if (wait32(hc.op, 2u, 0) < 0 || wait32(hc.op + 4, 1u << 11, 0) < 0)
        return -1;
    if (!(rd32(hc.op + 8) & 1u))
        return -1;

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
    cmd[XHCI_RING_TRBS - 1].control = TRB_TYPE(6) | TRB_ENT | 1;

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

int xhci_probe(struct pci_device *pdev)
{
    if (pdev->class_code != 0x0c || pdev->subclass != 0x03 ||
        pdev->prog_if != 0x30 || hc.mmio)
        return -1;
    uint64_t bar = pci_bar_addr(pdev, 0);
    if (!bar || (pdev->bar[0] & 1))
        return -1;
    uint32_t command_reg = pci_read32(pdev->bus, pdev->dev, pdev->func, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->func, 0x04,
                (command_reg & 0xFFFFu) | (1u << 1) | (1u << 2));
    hc.mmio = (volatile uint8_t *)vmm_mmap_phys(bar, 16,
                                                MMU_WRITE | MMU_UNCACHED);
    if (!hc.mmio || controller_start() < 0) {
        printk("xHCI: controller initialization failed\n");
        hc.mmio = 0;
        return -1;
    }
    for (uint8_t port = 1; port <= hc.max_ports; port++) {
        struct xhci_device *dev = &hc.devices[port - 1];
        dev->port = port;
        if (reset_port(port, &dev->speed) < 0)
            continue;
        printk("xHCI: port %u connected speed=%u\n", port, dev->speed);
        if (address_device(dev) < 0 || configure_device(dev) < 0) {
            printk("xHCI: port %u is not a supported HID boot device\n", port);
            continue;
        }
        queue_interrupt(dev);
    }
    hc.ready = 1;
    return 0;
}
