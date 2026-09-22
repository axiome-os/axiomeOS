#ifndef AXIOME_CLOCK_H
#define AXIOME_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the kernel wall-clock.  Must be called after hal_cpu_irq_enable()
   so that apic_get_ticks() is already incrementing.
   boot_unix    : Unix timestamp (UTC seconds since epoch) at boot.
   tz_offset_sec: seconds east of UTC (e.g. +10800 for UTC+3). */
void clock_init(uint64_t boot_unix, int64_t tz_offset_sec);

/* Monotonic nanoseconds since clock_init().  Never goes backwards. */
uint64_t clock_mono_ns(void);

/* Wall-clock seconds (local time = UTC + tz_offset). */
uint64_t clock_wall_sec(void);

/* Wall-clock with sub-second precision. */
void clock_wall_ns(uint64_t *sec_out, uint64_t *nsec_out);

/* Timezone offset (seconds east of UTC) as passed to clock_init(). */
int64_t clock_get_tz_offset(void);

/* UTC wall-clock (without timezone offset). */
uint64_t clock_wall_utc_sec(void);

/* Directly set the wall-clock to new_sec (local time). */
void clock_set_wall_sec(uint64_t new_sec);

/* Set wall-clock from an RTC UTC value (adds tz offset internally). */
void clock_set_utc_sec(uint64_t new_utc_sec);

/* Resync helpers: compare current time with rtc_utc and correct if
   abs drift >= threshold_sec. Returns 1 if corrected, 0 if within
   threshold, -1 if rtc invalid. */
int clock_resync_from_rtc(uint64_t rtc_utc_sec, uint64_t threshold_sec);

/* Drift in seconds between wall UTC and rtc UTC (signed). */
int64_t clock_drift_from_rtc(uint64_t rtc_utc_sec);

#ifdef __cplusplus
}
#endif

#endif
