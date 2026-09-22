#ifndef AXIOME_RTC_SYNC_H
#define AXIOME_RTC_SYNC_H

#include <stdint.h>

#define RTC_SYNC_INTERVAL_NS  (5000000000ULL)  /* 5 seconds */
#define RTC_SYNC_THRESHOLD_SEC 2               /* correct if drift >= 2s */

/* Spawn the background RTC sync thread. Must be called after clock_init()
   and sched_init() (so sched_spawn + sched_sleep_ns work). Idempotent. */
void rtc_sync_init(void);

/* Thread entry — not called directly; use rtc_sync_init(). */
void rtc_sync_thread(void *arg);

#endif