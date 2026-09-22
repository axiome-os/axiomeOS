#include "intel_display.h"
#include "../hal/hal.h"
extern "C" {
#include "../pci.h"
#include "../printk.h"
}

namespace gfx {

IntelDisplay::IntelDisplay() = default;

IntelDisplay *intel_display_create(void)
{
    static unsigned char storage[sizeof(IntelDisplay)];
    return new (storage) IntelDisplay();
}

bool IntelDisplay::probe()
{
    /* PCI display class (0x03), Intel vendor (0x8086). Walk the pci_init()
       list; if PCI is not up yet, report false and let GOP win. */
    struct pci_device *p = pci_first();
    for (; p; p = p->next) {
        if (p->vendor == 0x8086 && p->class_code == 0x03) {
            found_ = true;
            bus_ = p->bus;
            dev_ = p->dev;
            func_ = p->func;
            device_id_ = p->device;
            bar0_ = pci_bar_addr(p, 0);
            printk("gfx: intel iGPU %04x found at %02x:%02x.%u BAR0=0x%lx "
                   "(stub, scanout stays on GOP)\n",
                   device_id_, bus_, dev_, func_,
                   (unsigned long)bar0_);
            return true;
        }
    }
    return false;
}

bool IntelDisplay::init(const GfxMode & /*mode*/)
{
    /* Stage 1: deliberately refuse modeset. DisplayManager treats init()==
       false as "not ready" and falls through to the next backend (GOP),
       while probe()==true still advertises the hardware in logs and in
       gfx caps for Mesa planning. Flip this to true once GTT + pipe
       programming lands. */
    if (!found_)
        return false;
    shadow_.width = 0;
    shadow_.height = 0;
    shadow_.pitch = 0;
    shadow_.format = PixelFormat::Unknown;
    return false;
}

GfxCaps IntelDisplay::caps() const
{
    GfxCaps c;
    // Stub: no caps until GTT + engine init lands; then set fill/blit/alpha/compositor
    gfx_caps_sync_bits(c);
    return c;
}

void IntelDisplay::clear(uint32_t /*rgb*/) {}
void IntelDisplay::fill_rect(const GfxRect & /*r*/, uint32_t /*rgb*/) {}
void IntelDisplay::blit(const GfxRect & /*dst*/, const uint32_t * /*src*/,
                        uint32_t /*src_pitch_px*/)
{
}
void IntelDisplay::present() {}

} /* namespace gfx */
