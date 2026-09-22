#include "bochs_display.h"
#include "../hal/hal.h"
extern "C" {
#include "../pci.h"
#include "../printk.h"
}

namespace gfx {

BochsDisplay::BochsDisplay() = default;

BochsDisplay *bochs_display_create(void)
{
    static unsigned char storage[sizeof(BochsDisplay)];
    return new (storage) BochsDisplay();
}

bool BochsDisplay::probe()
{
    struct pci_device *p = pci_first();
    for (; p; p = p->next) {
        if (p->vendor == 0x1234 && p->device == 0x1111) {
            found_ = true;
            printk("gfx: bochs-vga at %02x:%02x.%u (stub, scanout stays on GOP)\n",
                   p->bus, p->dev, p->func);
            return true;
        }
    }
    return false;
}

bool BochsDisplay::init(const GfxMode & /*mode*/)
{
    /* Same staging contract as IntelDisplay: probe advertises, init refuses
       until a native modeset exists, manager falls back to GOP. */
    if (!found_)
        return false;
    shadow_.width = 0;
    shadow_.height = 0;
    shadow_.pitch = 0;
    shadow_.format = PixelFormat::Unknown;
    return false;
}

GfxCaps BochsDisplay::caps() const
{
    GfxCaps c;
    gfx_caps_sync_bits(c);
    return c;
}

void BochsDisplay::clear(uint32_t /*rgb*/) {}
void BochsDisplay::fill_rect(const GfxRect & /*r*/, uint32_t /*rgb*/) {}
void BochsDisplay::blit(const GfxRect & /*dst*/, const uint32_t * /*src*/,
                        uint32_t /*src_pitch_px*/)
{
}
void BochsDisplay::present() {}

} /* namespace gfx */
