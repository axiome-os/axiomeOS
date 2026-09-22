#include "cmos.h"
#include "printk.h"
#include "spinlock.h"
#include "hal/cshim.h"
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS   0x00
#define RTC_MINUTES   0x02
#define RTC_HOURS     0x04
#define RTC_WEEKDAY   0x06
#define RTC_DAY       0x07
#define RTC_MONTH     0x08
#define RTC_YEAR      0x09
#define RTC_CENTURY   0x32
#define RTC_STATUS_A  0x0A
#define RTC_STATUS_B  0x0B

#define RTC_UIP       0x80
#define RTC_24H       0x02
#define RTC_BCD       0x04

static spinlock_t cmos_lock = SPINLOCK_INIT;

static uint8_t cmos_port_read(uint8_t reg)
{
    /* Preserve NMI disable bit (bit 7 of port 0x70). We keep NMIs enabled
       by clearing bit 7; the HAL port helper does raw outb. */
    unsigned long flags = spin_lock_irq(&cmos_lock);
    hal_port_out8(CMOS_ADDR, reg & 0x7F);
    hal_port_delay();
    uint8_t v = hal_port_in8(CMOS_DATA);
    hal_port_delay();
    spin_unlock_irq(&cmos_lock, flags);
    return v;
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

uint64_t cmos_to_unix(int year, int mon, int day,
                      int hour, int min, int sec)
{
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y = year - 1970;
    uint64_t days = (uint64_t)y * 365
                  + (uint64_t)((y + 1) / 4)
                  - (uint64_t)((y + 69) / 100)
                  + (uint64_t)((y + 369) / 400);
    int is_leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
    for (int m = 1; m < mon; m++) {
        days += mdays[m - 1];
        if (m == 2 && is_leap) days++;
    }
    days += (uint64_t)(day - 1);
    return days * 86400ULL
         + (uint64_t)hour * 3600ULL
         + (uint64_t)min  * 60ULL
         + (uint64_t)sec;
}

static int wait_not_uip(void)
{
    /* Timeout to avoid infinite loop on broken HW: ~10ms busy wait */
    for (int i = 0; i < 100000; i++) {
        if (!(cmos_port_read(RTC_STATUS_A) & RTC_UIP))
            return 0;
        hal_cpu_pause();
    }
    return -1;
}

int cmos_read_time(struct cmos_rtc_time *out)
{
    if (!out) return -1;

    if (wait_not_uip() != 0)
        return -1;

    uint8_t sec  = cmos_port_read(RTC_SECONDS);
    uint8_t min  = cmos_port_read(RTC_MINUTES);
    uint8_t hour = cmos_port_read(RTC_HOURS);
    uint8_t day  = cmos_port_read(RTC_DAY);
    uint8_t mon  = cmos_port_read(RTC_MONTH);
    uint8_t year = cmos_port_read(RTC_YEAR);
    uint8_t cent = cmos_port_read(RTC_CENTURY);
    uint8_t statb = cmos_port_read(RTC_STATUS_B);

    /* Confirm no update occurred mid-read; if UIP or second changed re-read. */
    if (cmos_port_read(RTC_STATUS_A) & RTC_UIP) {
        if (wait_not_uip() != 0) return -1;
        sec  = cmos_port_read(RTC_SECONDS);
        min  = cmos_port_read(RTC_MINUTES);
        hour = cmos_port_read(RTC_HOURS);
        day  = cmos_port_read(RTC_DAY);
        mon  = cmos_port_read(RTC_MONTH);
        year = cmos_port_read(RTC_YEAR);
        cent = cmos_port_read(RTC_CENTURY);
        statb = cmos_port_read(RTC_STATUS_B);
    } else {
        /* Double-read seconds/minutes to detect rollover between registers */
        uint8_t sec2  = cmos_port_read(RTC_SECONDS);
        uint8_t min2  = cmos_port_read(RTC_MINUTES);
        uint8_t hour2 = cmos_port_read(RTC_HOURS);
        uint8_t day2  = cmos_port_read(RTC_DAY);
        uint8_t mon2  = cmos_port_read(RTC_MONTH);
        uint8_t year2 = cmos_port_read(RTC_YEAR);
        uint8_t cent2 = cmos_port_read(RTC_CENTURY);
        if (sec2 != sec || min2 != min || hour2 != hour ||
            day2 != day || mon2 != mon || year2 != year) {
            sec = sec2; min = min2; hour = hour2;
            day = day2; mon = mon2; year = year2; cent = cent2;
        }
    }

    /* Status B bit 2 (RTC_BCD 0x04): 1 = binary, 0 = BCD.
       Preserve AM/PM flag before masking 0x80, otherwise 12h PM is lost. */
    bool is_pm = false;
    if (statb & RTC_BCD) {
        /* Binary mode: values already binary, just preserve PM bit */
        is_pm = (hour & 0x80) != 0;
        hour &= 0x7F;
    } else {
        /* BCD mode: decode, preserving PM bit before clearing 0x80 */
        is_pm = (hour & 0x80) != 0;
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        cent = bcd_to_bin(cent);
    }

    if (!(statb & RTC_24H)) {
        /* 12-hour mode: apply AM/PM using preserved flag */
        if (is_pm) {
            if (hour != 12) hour += 12;
        } else {
            if (hour == 12) hour = 0;
        }
    }

    int full_year;
    if (cent != 0)
        full_year = (int)cent * 100 + (int)year;
    else
        full_year = (year >= 70) ? (1900 + (int)year) : (2000 + (int)year);

    if (full_year < 2020 || full_year > 2100 ||
        mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        return -1;
    }

    out->year = full_year;
    out->mon  = mon;
    out->day  = day;
    out->hour = hour;
    out->min  = min;
    out->sec  = sec;
    return 0;
}

uint64_t cmos_read_unix(void)
{
    struct cmos_rtc_time tm;
    if (cmos_read_time(&tm) != 0)
        return 0;
    return cmos_to_unix(tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
}

int cmos_init(void)
{
    struct cmos_rtc_time tm;
    int r = cmos_read_time(&tm);
    if (r == 0) {
        printk("CMOS: RTC %04d-%02d-%02d %02d:%02d:%02d UTC\n",
               tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
        uint64_t u = cmos_to_unix(tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
        printk("CMOS: unix=%lu\n", (unsigned long)u);
        return 0;
    }
    printk("CMOS: RTC probe failed (no valid time)\n");
    return -1;
}
