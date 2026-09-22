#include "rtc_sync.h"
#include "cmos.h"
#include "clock.h"
#include "sched.h"
#include "printk.h"

static int g_started = 0;

void rtc_sync_init(void)
{
    if (g_started)
        return;
    g_started = 1;

    if (cmos_init() != 0) {
        printk("RTC sync: CMOS probe failed at init\n");
    }

    uint64_t rtc = cmos_read_unix();
    if (!rtc == 0) {
        clock_set_utc_sec(rtc);
    }
    
    printk("RTC sync: synced startup\n");
}
