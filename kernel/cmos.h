#ifndef AXIOME_CMOS_H
#define AXIOME_CMOS_H

#include <stdint.h>

/* Broken-down RTC time as read from CMOS. year is full 4-digit (e.g. 2026). */
struct cmos_rtc_time {
    int year;
    int mon;   /* 1..12 */
    int day;   /* 1..31 */
    int hour;  /* 0..23 */
    int min;   /* 0..59 */
    int sec;   /* 0..59 */
};

/* Initialise the CMOS driver. Currently a no-op sanity probe; returns 0 on
   success, <0 if RTC appears absent/broken. Safe to call multiple times. */
int cmos_init(void);

/* Read raw RTC fields. Returns 0 on success, -1 if values implausible or
   RTC update-in-progress could not be resolved. */
int cmos_read_time(struct cmos_rtc_time *out);

/* Read RTC as Unix UTC seconds. Returns 0 if unavailable/invalid (caller
   must treat 0 as failure — 1970-01-01 00:00:00 never occurs as real RTC). */
uint64_t cmos_read_unix(void);

/* Convert a calendar date to Unix timestamp (UTC, no timezone). */
uint64_t cmos_to_unix(int year, int mon, int day,
                      int hour, int min, int sec);

/* Legacy alias kept for compatibility with older code that included rtc_efi.h */
static inline uint64_t cmos_get_unix(void) { return cmos_read_unix(); }

#endif
