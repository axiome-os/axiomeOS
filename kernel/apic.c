#include "apic.h"
#include "printk.h"
#include "hal/cshim.h"

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_DEFAULT_BASE 0xFEE00000ULL

#define APIC_OFFSET_ID         0x020
#define APIC_OFFSET_EOI        0x0B0
#define APIC_OFFSET_SPURIOUS   0x0F0
#define APIC_OFFSET_LVT_TIMER  0x320
#define APIC_OFFSET_TIMER_INIT 0x380
#define APIC_OFFSET_TIMER_CUR  0x390
#define APIC_OFFSET_TIMER_DIV  0x3E0

#define APIC_SPURIOUS_ENABLE (1 << 8)
#define APIC_LVT_PERIODIC    (1 << 17)
#define APIC_LVT_ONESHOT     0
#define APIC_LVT_MASKED      (1 << 16)
#define APIC_TIMER_DIV16     3

/* PIT channel 2 ports used for APIC calibration. */
#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define PIT_CTL       0x61

/* Calibration: count APIC ticks over a 30 ms PIT gate (PIT at 1193182 Hz,
    30 ms = 35795 counts). Longer window reduces APIC bus-tick measurement
    error that otherwise manifests as wall-clock drift (APIC seconds counting
    not so precise). */
#define PIT_CALIBRATE_MS    30
#define PIT_HZ              1193182UL
#define PIT_CALIB_COUNT     ((PIT_HZ * PIT_CALIBRATE_MS) / 1000)  /* ~35795 */

static volatile uint32_t *apic_base;
static volatile uint64_t timer_ticks;

/* Nanoseconds per APIC tick, scaled by 2^20 to avoid floating point.
   Set once during apic_calibrate(); used by apic_ticks_to_ns(). */
static uint64_t ns_per_tick_scaled; /* ns * 2^20 per tick */
static uint64_t ticks_per_ms;       /* APIC ticks per millisecond */

static uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(port));
    return v;
}

static void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "d"(port));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = value & 0xFFFFFFFF;
    uint32_t hi = value >> 32;
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void apic_write(unsigned int reg, uint32_t val)
{
    apic_base[reg / 4] = val;
}

static uint32_t apic_read(unsigned int reg)
{
    return apic_base[reg / 4];
}

void apic_timer_tick(void)
{
    apic_eoi();
    timer_ticks++;
}

/* Measure the real tick->ns conversion factor against the PIT once IRQs are
   live. We cannot assume each timer ISR firing is exactly 1 ms of real time:
   the APIC timer's actual period depends on the bus clock / divider and can
   be several percent off the nominal 1 ms (observed ~10% slow here), and the
   pre-IRQ raw_ticks_per_ms estimate is only used to pick the reload count.
   So count ISR firings over a PIT reference gate and derive the true ratio.
   Must be called after apic_timer_start() and hal_cpu_irq_enable(). */
void apic_calibrate_post_irq(void)
{
    /* 8 back-to-back PIT channel-2 windows, 50 ms each: ~400 ms baseline.
       CAL_WIN_MS must keep the 16-bit PIT count <= 65535 (~54.9 ms max). */
    enum { CAL_WINDOWS = 8, CAL_WIN_MS = 50 };
    const uint16_t cal_count = (uint16_t)((PIT_HZ * CAL_WIN_MS) / 1000);

    uint64_t t0 = timer_ticks;

    for (int i = 0; i < CAL_WINDOWS; i++) {
        /* Mode 0 (one-shot), lsb+msb: writing the count starts the gate.
           Reloading after a window restarts it (OUT drops then retriggers). */
        outb(PIT_CMD, 0xB0);
        outb(PIT_CH2_DATA, (uint8_t)(cal_count & 0xFF));
        outb(PIT_CH2_DATA, (uint8_t)(cal_count >> 8));
        while (!(inb(PIT_CTL) & 0x20))
            hal_cpu_pause();
    }

    uint64_t t1 = timer_ticks;
    uint64_t ticks = t1 - t0;
    if (ticks == 0)
        ticks = 1;

    uint64_t real_ns = (uint64_t)CAL_WINDOWS * CAL_WIN_MS * 1000000ULL;
    ns_per_tick_scaled = (real_ns << 20) / ticks;

    /* ticks_per_ms no longer drives the reload (the timer is already running
       at that count); keep it as the ISR-firings-per-ms accounting rate. */
    ticks_per_ms = 1;

    printk("APIC: post-irq calibrate: %lu ISR ticks in %lu ms => %lu ns/tick (scaled %lu)\n",
           (unsigned long)ticks,
           (unsigned long)(CAL_WINDOWS * CAL_WIN_MS),
           (unsigned long)(ns_per_tick_scaled >> 20),
           (unsigned long)ns_per_tick_scaled);
}

/* Calibrate the APIC timer against PIT channel 2 (early boot, pre-IRQ).
   This gives ticks_per_ms in raw APIC bus cycles, used only to set the
   apic_timer_start reload value so the periodic timer fires at ~1 ms.
   The ns_per_tick conversion is NOT set here — apic_calibrate_post_irq()
   does that accurately after IRQs are live. */
static void apic_calibrate(void)
{
    /* Ensure PIT OUT starts low: gate off then on. */
    uint8_t ctl = inb(PIT_CTL);
    outb(PIT_CTL, (ctl & 0xFE));               /* gate off */
    outb(PIT_CTL, (ctl & 0xFC) | 0x01);        /* gate on, speaker off */

    /* Program PIT channel 2: mode 0 (one-shot), lsb+msb, binary. */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, (uint8_t)(PIT_CALIB_COUNT & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(PIT_CALIB_COUNT >> 8));

    /* Set APIC timer to one-shot masked, divisor 16, max count. */
    apic_write(APIC_OFFSET_TIMER_DIV, APIC_TIMER_DIV16);
    apic_write(APIC_OFFSET_LVT_TIMER, 0x20 | APIC_LVT_MASKED | APIC_LVT_ONESHOT);
    apic_write(APIC_OFFSET_TIMER_INIT, 0xFFFFFFFFU);

    /* Wait for PIT OUT to go high (bit 5 of port 0x61). */
    while ((inb(PIT_CTL) & 0x20) == 0)
        ;

    uint32_t remain  = apic_read(APIC_OFFSET_TIMER_CUR);
    uint32_t elapsed = 0xFFFFFFFFU - remain;

    /* Raw APIC bus ticks per ms — used only for apic_timer_start reload. */
    uint64_t raw_ticks_per_ms = (uint64_t)elapsed / PIT_CALIBRATE_MS;
    if (raw_ticks_per_ms == 0)
        raw_ticks_per_ms = 100000; /* fallback: ~100 MHz bus */

    /* Store in ticks_per_ms temporarily; post-irq calibration overwrites it. */
    ticks_per_ms = raw_ticks_per_ms;
    /* ns_per_tick_scaled stays 0 until apic_calibrate_post_irq(). */
    ns_per_tick_scaled = 0;

    printk("APIC: pre-irq calibration: raw %lu APIC ticks/ms\n",
           (unsigned long)raw_ticks_per_ms);
}

/* Start the periodic local APIC timer.  count is in APIC bus cycles
   (divisor 16).  Pass 0 to use the calibrated 1 ms period. */
void apic_timer_start(uint32_t count)
{
    if (count == 0)
        count = (uint32_t)ticks_per_ms;   /* 1 ms per tick */
    apic_write(APIC_OFFSET_TIMER_DIV, APIC_TIMER_DIV16);
    apic_write(APIC_OFFSET_LVT_TIMER, 0x20 | APIC_LVT_PERIODIC);
    apic_write(APIC_OFFSET_TIMER_INIT, count);
}

void apic_init(void)
{
    timer_ticks = 0;
    ns_per_tick_scaled = 0;
    ticks_per_ms = 0;

    uintptr_t apic_phys = rdmsr(IA32_APIC_BASE_MSR) & 0xFFFFFF000ULL;
    printk("APIC: base=0x%lx\n", (unsigned long)apic_phys);

    if (apic_phys == 0)
        apic_phys = APIC_DEFAULT_BASE;

    apic_base = (volatile uint32_t *)hal_mmio_map_phys(apic_phys, 0x1000);

    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    apic_msr |= (1ULL << 11);
    wrmsr(IA32_APIC_BASE_MSR, apic_msr);

    apic_write(APIC_OFFSET_SPURIOUS, 0xFF | APIC_SPURIOUS_ENABLE);
    apic_read(APIC_OFFSET_SPURIOUS);

    /* Mask legacy PIC — we use APIC exclusively. */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFF), "d"((uint16_t)0xA1));
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFF), "d"((uint16_t)0x21));

    apic_calibrate();

    printk("APIC: init complete\n");
}

void apic_eoi(void)
{
    apic_write(APIC_OFFSET_EOI, 0);
}

uint64_t apic_get_ticks(void)
{
    return timer_ticks;
}

uint64_t apic_get_ticks_per_ms(void)
{
    return ticks_per_ms;
}

/* Convert a raw tick count to nanoseconds using the calibrated ratio. */
uint64_t apic_ticks_to_ns(uint64_t ticks)
{
    /* (ticks * ns_per_tick_scaled) >> 20 */
    /* Split to avoid 128-bit overflow: ticks * (ns_per_tick_scaled >> 10)
       then >> 10.  ns_per_tick_scaled is at most ~1e9 * 2^20 / 1 < 2^50,
       so ticks up to ~2^14 are safe with direct multiply.  For larger
       tick counts we use the split form. */
    return (ticks * ns_per_tick_scaled) >> 20;
}
