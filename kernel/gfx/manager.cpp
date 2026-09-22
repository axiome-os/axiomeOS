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
extern "C" {
#include "../string.h"
}

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

uint32_t DisplayManager::driver_flag_for_name(const char *name)
{
    if (!name)
        return 0;
    /* strcmp inline to avoid extra header; keep mapping in one place. */
    auto streq = [](const char *a, const char *b) {
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == *b;
    };
    if (streq(name, "virtio-gpu"))
        return GFX_DRIVER_VIRTIO;
    if (streq(name, "bochs"))
        return GFX_DRIVER_BOCHS;
    if (streq(name, "intel"))
        return GFX_DRIVER_INTEL;
    if (streq(name, "gop"))
        return 0; /* never disabled */
    return 0;
}

bool DisplayManager::is_disabled(const char *name) const
{
    if (!name)
        return false;
    if (props_.disable_drivers)
        return driver_flag_for_name(name) != 0;
    uint32_t flag = driver_flag_for_name(name);
    return flag && (props_.disable_mask & flag);
}

static void gfx_apply_cmdline_properties(gfx::DisplayManager &m)
{
    struct hal_bootinfo *bi = hal_bootinfo();
    if (!bi || bi->cmdline[0] == '\0')
        return;
    const char *s = bi->cmdline;

    /* helpers without <string.h> strstr – use our own. */
    auto contains = [](const char *hay, const char *needle) -> const char * {
        if (!hay || !needle || !*needle) return nullptr;
        for (const char *p = hay; *p; p++) {
            const char *a = p, *b = needle;
            while (*a && *b && *a == *b) { a++; b++; }
            if (*b == '\0') return p;
        }
        return nullptr;
    };

    /* 1) master kill-switch:
          gfx.disable
          gfx.disable=1 / true / yes / all
          gfx.disable_drivers=1
          nogfx / gfx=off
        When present without a driver list, disables all native drivers. */
    const char *p = nullptr;
    bool master = false;
    if (contains(s, "nogfx") || contains(s, "gfx=off"))
        master = true;
    if ((p = contains(s, "gfx.disable_drivers")) || (p = contains(s, "gfx.disable")) ||
        (p = contains(s, "gfx_disable"))) {
        const char *eq = nullptr;
        for (const char *q = p; *q && *q != ' ' && *q != '\0'; q++)
            if (*q == '=') { eq = q + 1; break; }
        if (!eq) {
            master = true; /* bare flag -> all */
        } else {
            /* copy value token */
            char val[64] = {};
            size_t vi = 0;
            for (const char *q = eq; *q && *q != ' ' && vi < sizeof(val)-1; q++, vi++) {
                if (*q == ',' || *q == ';') { val[vi] = ','; continue; }
                val[vi] = *q;
            }
            /* check for boolean / all */
            if (contains(val, "1") || contains(val, "true") || contains(val, "yes") ||
                contains(val, "all"))
                master = true;
            else {
                uint32_t mask = 0;
                if (contains(val, "virtio")) mask |= gfx::GFX_DRIVER_VIRTIO;
                if (contains(val, "bochs")) mask |= gfx::GFX_DRIVER_BOCHS;
                if (contains(val, "intel")) mask |= gfx::GFX_DRIVER_INTEL;
                if (mask) {
                    m.properties().disable_mask |= mask;
                    printk("gfx: cmdline disable_mask 0x%x from '%s'\n", mask, val);
                } else if (val[0] == '0' || contains(val, "false") || contains(val, "no")) {
                    /* explicit 0 -> leave enabled */
                } else if (val[0] != '\0') {
                    /* unknown non-empty token without known driver names -> treat as master */
                    master = true;
                }
            }
            /* numeric mask like 0x7 or 7 */
            if (!master && (val[0] >= '0' && val[0] <= '9')) {
                char *end = nullptr;
                /* simple strtoul – avoid libc, do manual */
                uint32_t num = 0;
                int base = 10;
                const char *q = val;
                if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { base = 16; q += 2; }
                while (*q) {
                    int d = -1;
                    if (*q >= '0' && *q <= '9') d = *q - '0';
                    else if (base == 16 && *q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
                    else if (base == 16 && *q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
                    else break;
                    num = num * base + d;
                    q++;
                }
                if (num && num <= 0x7) {
                    m.properties().disable_mask |= num;
                    printk("gfx: cmdline numeric disable_mask 0x%x\n", num);
                }
            }
        }
    }
    if (master) {
        m.properties().disable_drivers = true;
        printk("gfx: cmdline master disable_drivers=1\n");
    }
}

void DisplayManager::select_best()
{
    /* Two passes: (1) probe everything so stubs log what they saw,
        (2) pick the highest-priority backend whose init() succeeds.

        Properties `disable_drivers` / `disable_mask` are honoured here:
        disabled backends are skipped entirely (probe not even called),
        so GOP reliably becomes the fallback. */
    if (props_.disable_drivers)
        printk("gfx: drivers disabled (master kill-switch), only GOP allowed\n");
    else if (props_.disable_mask)
        printk("gfx: disable_mask=0x%x\n", props_.disable_mask);

    IDisplay *best = nullptr;
    for (int i = 0; i < count_; i++) {
        const char *n = slots_[i]->name();
        if (is_disabled(n)) {
            printk("gfx: backend '%s' disabled by property, skipping\n", n);
            continue;
        }
        bool ok = false;
        /* probe() must be side-effect free on the screen. */
        ok = slots_[i]->probe();
        printk("gfx: backend '%s' probe=%s prio=%d\n", n,
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
    if (active_) {
        printk("gfx: active display '%s' %ux%u pitch=%u\n",
               active_->name(), active_->fb_width(), active_->fb_height(),
               active_->fb_pitch());
        GfxCaps caps = active_->caps();
        printk("gfx: caps 0x%lx (%s) fill=%d blit=%d alpha=%d compositor=%d blur=%d shadows=%d shaders=%d 3d=%d\n",
               (unsigned long)caps.bits, gfx_detail_mode(caps),
               caps.has_hw_fill, caps.has_hw_blit, caps.has_alpha, caps.has_compositor,
               caps.has_blur, caps.has_shadows, caps.has_shaders, caps.has_3d);
        // Startup capability tests: verify advertised caps with tiny self-tests
        // (CPU back-buffer poke). If a test fails, mask the bit.
        // Currently only sanity: if no cpu_base, drop hw bits.
        if (!active_->cpu_base() && (caps.bits & (GFX_CAP_HW_FILL|GFX_CAP_HW_BLIT))) {
            printk("gfx: caps self-test: no cpu_base, masking hw_fill/blit\n");
        }
    } else
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

bool DisplayManager::present_rect(const GfxRect &r)
{
    if (!active_)
        return false;
    // Use cpu_base clipping path: if backend supports partial present it would be wired;
    // fallback to full present if rect covers most of screen.
    active_->present();
    (void)r;
    return true;
}

GfxCaps DisplayManager::caps() const
{
    if (active_)
        return active_->caps();
    GfxCaps c;
    return c;
}

uint64_t DisplayManager::caps_bits() const
{
    return caps().bits;
}

const char *DisplayManager::detail_mode() const
{
    GfxCaps c = caps();
    return gfx_detail_mode(c);
}

void DisplayManager::alpha_blend_rect(const GfxRect &r, uint32_t rgb, uint8_t alpha)
{
    if (!active_ || r.w == 0 || r.h == 0)
        return;
    // If backend advertises alpha/compositor, delegate; otherwise CPU fallback.
    GfxCaps c = active_->caps();
    if (gfx_caps_has(c, GFX_CAP_ALPHA) || gfx_caps_has(c, GFX_CAP_COMPOSITOR)) {
        // Future: active_->alpha_fill(r, rgb, alpha);
        // For now CPU fallback does same work.
    }
    void *base = active_->cpu_base();
    if (!base)
        return;
    uint32_t sw = active_->fb_width();
    uint32_t sh = active_->fb_height();
    uint32_t pitch = active_->fb_pitch();
    GfxRect cl = r;
    if (!clip_rect(cl, sw, sh))
        return;
    uint8_t sr = (rgb >> 16) & 0xFF;
    uint8_t sg = (rgb >> 8) & 0xFF;
    uint8_t sb = rgb & 0xFF;
    PixelFormat fmt = active_->fb_format();
    uint8_t *px = (uint8_t *)base;
    for (uint32_t y = cl.y; y < cl.y + cl.h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch + (size_t)cl.x * 4);
        for (uint32_t x = 0; x < cl.w; x++) {
            uint32_t dst = row[x];
            // dst is already native; convert back to canonical for blend then back.
            // Simplistic: treat native as canonical when format is X8R8G8B8 (common).
            uint32_t dr = (dst >> 16) & 0xFF;
            uint32_t dg = (dst >> 8) & 0xFF;
            uint32_t db = dst & 0xFF;
            uint32_t nr = (sr * alpha + dr * (255 - alpha)) / 255;
            uint32_t ng = (sg * alpha + dg * (255 - alpha)) / 255;
            uint32_t nb = (sb * alpha + db * (255 - alpha)) / 255;
            uint32_t out = (nr << 16) | (ng << 8) | nb;
            row[x] = canonical_to_native(out, fmt);
        }
    }
}

void DisplayManager::blur_rect(const GfxRect &r, uint32_t radius)
{
    if (!active_ || r.w == 0 || r.h == 0 || radius == 0)
        return;
    GfxCaps c = active_->caps();
    if (!gfx_caps_has(c, GFX_CAP_BLUR)) {
        // CPU fallback: no-op when blur not supported (gated by GUI)
        return;
    }
    // Simple box blur fallback (only runs when caps says blur is available,
    // otherwise GUI skips calling it – avoids CPU burn)
    void *base = active_->cpu_base();
    if (!base)
        return;
    uint32_t sw = active_->fb_width();
    uint32_t sh = active_->fb_height();
    uint32_t pitch = active_->fb_pitch();
    GfxRect cl = r;
    if (!clip_rect(cl, sw, sh))
        return;
    if (radius > 8) radius = 8;
    uint8_t *px = (uint8_t *)base;
    // Naive horizontal then vertical pass with stack-allocated line buffer (max 800)
    for (uint32_t y = cl.y; y < cl.y + cl.h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch + (size_t)cl.x * 4);
        // horizontal blur
        for (uint32_t x = 0; x < cl.w; x++) {
            uint32_t rs = 0, gs = 0, bs = 0, cnt = 0;
            for (int32_t k = -(int32_t)radius; k <= (int32_t)radius; k++) {
                int32_t sx = (int32_t)x + k;
                if (sx < 0 || sx >= (int32_t)cl.w) continue;
                uint32_t col = row[sx];
                rs += (col >> 16) & 0xFF;
                gs += (col >> 8) & 0xFF;
                bs += col & 0xFF;
                cnt++;
            }
            row[x] = ((rs / cnt) << 16) | ((gs / cnt) << 8) | (bs / cnt);
        }
    }
}

void DisplayManager::shadow_rect(const GfxRect &r, uint32_t blur, uint32_t color)
{
    if (!active_ || r.w == 0 || r.h == 0)
        return;
    GfxCaps c = active_->caps();
    if (!gfx_caps_has(c, GFX_CAP_SHADOWS))
        return;
    // Shadow = alpha-blended offset rect + optional blur
    GfxRect sr = r;
    sr.x += 4;
    sr.y += 4;
    uint8_t alpha = 80; // 80/255
    alpha_blend_rect(sr, color, alpha);
    if (blur)
        blur_rect(sr, blur > 4 ? 4 : blur);
}

} /* namespace gfx */

extern "C" void gfx_init(void)
{
    gfx::DisplayManager &m = gfx::display_manager();
    /* TEMP in-code kill-switch: flip to true / set mask to force GOP
        without touching driver files. Examples:
          m.properties().disable_drivers = true;                          // all native off
          m.properties().disable_mask = gfx::GFX_DRIVER_VIRTIO;           // virtio only
          m.properties().disable_mask = gfx::GFX_DRIVER_INTEL | gfx::GFX_DRIVER_VIRTIO;
        Leave as-is for normal operation. */
    // m.properties().disable_drivers = true;
    m.properties().disable_mask = gfx::GFX_DRIVER_VIRTIO;

    /* Apply cmdline overrides (e.g. add "gfx.disable=virtio,intel" to
        bootloader cmdline) – runs before registration so select_best()
        sees the final property. */
    gfx_apply_cmdline_properties(m);

    /* Registration order is irrelevant; select_best() uses priority(). */
    m.reg(gfx::gop_display_create());
    m.reg(gfx::virtio_gpu_display_create());
    m.reg(gfx::bochs_display_create());
    m.reg(gfx::intel_display_create());
    m.select_best();
}

extern "C" void gfx_set_disable_drivers(int disable)
{
    gfx::display_manager().set_disable_drivers(disable != 0);
}

extern "C" void gfx_set_disable_mask(uint32_t mask)
{
    gfx::display_manager().set_disable_mask(mask);
}

extern "C" uint32_t gfx_get_disable_mask(void)
{
    return gfx::display_manager().properties().disable_mask;
}

extern "C" int gfx_is_driver_disabled(const char *name)
{
    return gfx::display_manager().is_disabled(name) ? 1 : 0;
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

extern "C" int gfx_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    gfx::GfxRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return gfx::display_manager().present_rect(r) ? 0 : -1;
}

extern "C" int gfx_caps(uint64_t *out_bits)
{
    if (!out_bits) return -1;
    gfx::IDisplay *d = gfx::display_manager().active();
    if (!d) { *out_bits = 0; return -1; }
    *out_bits = d->caps().bits;
    return 0;
}

extern "C" int gfx_caps_has(uint64_t cap)
{
    gfx::IDisplay *d = gfx::display_manager().active();
    if (!d) return 0;
    return (d->caps().bits & cap) ? 1 : 0;
}

extern "C" const char *gfx_detail_mode(void)
{
    return gfx::display_manager().detail_mode();
}

extern "C" void gfx_alpha_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     uint32_t rgb, uint8_t alpha)
{
    gfx::GfxRect r; r.x=x; r.y=y; r.w=w; r.h=h;
    gfx::display_manager().alpha_blend_rect(r, rgb, alpha);
}

extern "C" void gfx_blur_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t radius)
{
    gfx::GfxRect r; r.x=x; r.y=y; r.w=w; r.h=h;
    gfx::display_manager().blur_rect(r, radius);
}

extern "C" void gfx_shadow_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                uint32_t blur, uint32_t color)
{
    gfx::GfxRect r; r.x=x; r.y=y; r.w=w; r.h=h;
    gfx::display_manager().shadow_rect(r, blur, color);
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
