#include "rtc_efi.h"
#include "cmos.h"
#include "printk.h"

/* Compatibility shim: the original rtc_efi driver read the CMOS directly.
   Now the dedicated CMOS driver (kernel/cmos.c) owns the hardware; this
   file only caches the boot-time value and forwards live reads to cmos. */

static uint64_t g_unix_time = 0;

void rtc_efi_set_systable(uint64_t systable_phys)
{
    (void)systable_phys;
}

void rtc_efi_init(void)
{
    g_unix_time = cmos_read_unix();
    if (g_unix_time != 0) {
        struct cmos_rtc_time tm;
        if (cmos_read_time(&tm) == 0)
            printk("RTC: %04d-%02u-%02u %02u:%02u:%02u (UTC) via CMOS\n",
                   tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
        printk("RTC: boot epoch=%lu\n", (unsigned long)g_unix_time);
    } else {
        printk("RTC: CMOS read failed, boot epoch unavailable\n");
    }
}

uint64_t rtc_efi_get_unix(void)
{
    if (g_unix_time != 0)
        return g_unix_time;
    /* Fallback to live read if init not yet called or failed */
    return cmos_read_unix();
}
