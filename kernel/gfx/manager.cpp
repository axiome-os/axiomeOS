#include "manager.h"
#include "../hal/hal.h"
extern "C" {
#include "../printk.h"
}
#include "gop_display.h"
#include "intel_display.h"
#include "bochs_display.h"
#include "virtio_gpu_display.h"
#include "../printk.h"
#include "../hal/hal_bootinfo.h"

namespace gfx {

DisplayManager &display_manager(void)
{
    static unsigned char storage[sizeof(DisplayManager)];
    static DisplayManager *inst = nullptr;
    if (!inst)
        inst = new (storage) DisplayManager();
    return *inst;
}

bool DisplayManager::reg(IDisplay *d)
{
    if (!d || count_ >= kMaxDisplays)
        return false;
    slots_[count_++] = d;
    return true;
}

void DisplayManager::select_best()
{
    /* Two passes: (1) probe everything so stubs log what they saw,
       (2) pick the highest-priority backend whose init() succeeds. */
    IDisplay *best = nullptr;
    for (int i = 0; i < count_; i++) {
        bool ok = false;
        /* probe() must be side-effect free on the screen. */
        ok = slots_[i]->probe();
        printk("gfx: backend '%s' probe=%s prio=%d\n", slots_[i]->name(),
               ok ? "yes" : "no", slots_[i]->priority());
        if (!ok)
            continue;
        GfxMode want; /* zeros = keep firmware mode */
        if (!slots_[i]->init(want))
            continue;
        if (!best || slots_[i]->priority() > best->priority())
            best = slots_[i];
    }
    active_ = best;
    if (active_)
        printk("gfx: active display '%s' %ux%u pitch=%u\n",
               active_->name(), active_->fb_width(), active_->fb_height(),
               active_->fb_pitch());
    else
        printk("gfx: no display backend ready\n");
}

const char *DisplayManager::active_name() const
{
    return active_ ? active_->name() : "none";
}

void DisplayManager::clear(uint32_t rgb)
{
    if (active_)
        active_->clear(rgb);
}

void DisplayManager::fill_rect(const GfxRect &r, uint32_t rgb)
{
    if (active_)
        active_->fill_rect(r, rgb);
}

void DisplayManager::present()
{
    if (active_)
        active_->present();
}

} /* namespace gfx */

extern "C" void gfx_init(void)
{
    gfx::DisplayManager &m = gfx::display_manager();
    /* Registration order is irrelevant; select_best() uses priority(). */
    m.reg(gfx::gop_display_create());
    m.reg(gfx::virtio_gpu_display_create());
    m.reg(gfx::bochs_display_create());
    m.reg(gfx::intel_display_create());
    m.select_best();
}

extern "C" int gfx_active(void)
{
    return gfx::display_manager().active() != nullptr;
}

extern "C" const char *gfx_active_name(void)
{
    return gfx::display_manager().active_name();
}

extern "C" void gfx_clear(uint32_t rgb)
{
    gfx::display_manager().clear(rgb);
}

extern "C" void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t rgb)
{
    gfx::GfxRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    gfx::display_manager().fill_rect(r, rgb);
}

extern "C" void gfx_present(void)
{
    gfx::display_manager().present();
}

extern "C" int gfx_mode(uint32_t *w, uint32_t *h, uint32_t *pitch,
                        uint32_t *bpp)
{
    gfx::IDisplay *d = gfx::display_manager().active();
    if (!d)
        return -1;
    if (w)
        *w = d->fb_width();
    if (h)
        *h = d->fb_height();
    if (pitch)
        *pitch = d->fb_pitch();
    if (bpp)
        *bpp = gfx::pixel_format_bpp(d->fb_format());
    return 0;
}

extern "C" void *gfx_cpu_base(void)
{
    gfx::IDisplay *d = gfx::display_manager().active();
    if (!d)
        return nullptr;
    return d->cpu_base();
}
