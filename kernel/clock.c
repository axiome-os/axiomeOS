#include "clock.h"
#include "apic.h"
#include "printk.h"

/* Snapshot taken at clock_init() time. */
static uint64_t boot_tick;           /* apic_get_ticks() value at init        */
static uint64_t boot_base_local_ns;  /* local wall ns at mono = 0 (UTC + tz)  */
static int64_t  boot_tz_offset;      /* seconds east of UTC                   */

void clock_init(uint64_t boot_unix, int64_t tz_offset_sec)
{
    boot_tz_offset     = tz_offset_sec;
    boot_tick          = apic_get_ticks();
    /* boot_base is the local wall time (UTC + tz) at mono 0, in ns */
    boot_base_local_ns = (uint64_t)((int64_t)boot_unix + tz_offset_sec) * 1000000000ULL;
}

/* Nanoseconds elapsed since clock_init(). */
uint64_t clock_mono_ns(void)
{
    uint64_t elapsed = apic_get_ticks() - boot_tick;
    return apic_ticks_to_ns(elapsed);
}

uint64_t clock_wall_sec(void)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    return wall_ns / 1000000000ULL;
}

void clock_wall_ns(uint64_t *sec_out, uint64_t *nsec_out)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    if (sec_out)  *sec_out  = wall_ns / 1000000000ULL;
    if (nsec_out) *nsec_out = wall_ns % 1000000000ULL;
}

int64_t clock_get_tz_offset(void)
{
    return boot_tz_offset;
}

uint64_t clock_wall_utc_sec(void)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    int64_t wall_sec = (int64_t)(wall_ns / 1000000000ULL);
    return (uint64_t)(wall_sec - boot_tz_offset);
}

void clock_set_wall_sec(uint64_t new_sec)
{
    uint64_t mono = clock_mono_ns();
    boot_base_local_ns = new_sec * 1000000000ULL - mono;
    printk("Clock: corrected wall -> %lu (tz %ld)\n",
           (unsigned long)new_sec, (long)boot_tz_offset);
}

void clock_set_utc_sec(uint64_t new_utc_sec)
{
    uint64_t new_wall = (uint64_t)((int64_t)new_utc_sec + boot_tz_offset);
    clock_set_wall_sec(new_wall);
}

int64_t clock_drift_from_rtc(uint64_t rtc_utc_sec)
{
    uint64_t utc = clock_wall_utc_sec();
    return (int64_t)utc - (int64_t)rtc_utc_sec;
}

int clock_resync_from_rtc(uint64_t rtc_utc_sec, uint64_t threshold_sec)
{
    if (rtc_utc_sec == 0)
        return -1;
    int64_t drift = clock_drift_from_rtc(rtc_utc_sec);
    int64_t abs_drift = drift < 0 ? -drift : drift;
    if ((uint64_t)abs_drift >= threshold_sec) {
        clock_set_utc_sec(rtc_utc_sec);
        printk("Clock: drift %ld s >= %lu, resynced to RTC %lu\n",
               (long)drift, (unsigned long)threshold_sec,
               (unsigned long)rtc_utc_sec);
        return 1;
    }
    return 0;
}
