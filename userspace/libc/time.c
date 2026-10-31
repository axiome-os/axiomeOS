#include "time.h"
#include "syscall.h"
#include "errno.h"

/* Static storage for localtime/gmtime results */
static struct tm _tm_buf;

time_t time(time_t *tloc)
{
    long t = syscall(SYS_TIME, (long)tloc, 0, 0, 0, 0, 0);
    if (tloc)
        *tloc = (time_t)t;
    return (time_t)t;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    return (int)syscall(SYS_GETTIMEOFDAY, (long)tv, 0, 0, 0, 0, 0);
}

/* Basic implementation - assumes UNIX epoch */
static void time_to_tm(time_t t, struct tm *tm)
{
    /* Simple algorithm for converting UNIX timestamp to broken-down time */
    long days = t / 86400;
    long rem = t % 86400;
    
    tm->tm_hour = (int)(rem / 3600);
    rem %= 3600;
    tm->tm_min = (int)(rem / 60);
    tm->tm_sec = (int)(rem % 60);
    
    /* Start from 1970-01-01 (Thursday) */
    tm->tm_wday = (int)((days + 4) % 7);
    
    /* Calculate year (approximate - ignores leap seconds) */
    int year = 1970;
    long year_days;
    
    for (;;)
    {
        int is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        year_days = is_leap ? 366 : 365;
        if (days < year_days)
            break;
        days -= year_days;
        year++;
    }
    
    tm->tm_year = year - 1900;
    tm->tm_yday = (int)days;
    
    /* Calculate month and day */
    int is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    static const int month_days[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
    };
    
    int month = 0;
    while (month < 12 && days >= month_days[is_leap][month])
    {
        days -= month_days[is_leap][month];
        month++;
    }
    
    tm->tm_mon = month;
    tm->tm_mday = (int)days + 1;
    tm->tm_isdst = 0;
}

struct tm *localtime(const time_t *timep)
{
    if (!timep)
        return NULL;
    time_to_tm(*timep, &_tm_buf);
    return &_tm_buf;
}

struct tm *gmtime(const time_t *timep)
{
    /* For now, same as localtime (no timezone support) */
    return localtime(timep);
}

char *asctime(const struct tm *tm)
{
    static char buf[26];
    static const char wday_name[7][4] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char mon_name[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    
    if (!tm)
        return NULL;
    
    /* Format: "Wed Jun 30 21:49:08 1993\n" */
    char *p = buf;
    const char *wd = wday_name[tm->tm_wday % 7];
    const char *mn = mon_name[tm->tm_mon % 12];
    
    *p++ = wd[0]; *p++ = wd[1]; *p++ = wd[2]; *p++ = ' ';
    *p++ = mn[0]; *p++ = mn[1]; *p++ = mn[2]; *p++ = ' ';
    
    int mday = tm->tm_mday;
    *p++ = (mday >= 10) ? ('0' + mday / 10) : ' ';
    *p++ = '0' + (mday % 10);
    *p++ = ' ';
    
    *p++ = '0' + (tm->tm_hour / 10);
    *p++ = '0' + (tm->tm_hour % 10);
    *p++ = ':';
    *p++ = '0' + (tm->tm_min / 10);
    *p++ = '0' + (tm->tm_min % 10);
    *p++ = ':';
    *p++ = '0' + (tm->tm_sec / 10);
    *p++ = '0' + (tm->tm_sec % 10);
    *p++ = ' ';
    
    int year = tm->tm_year + 1900;
    *p++ = '0' + (year / 1000);
    *p++ = '0' + ((year / 100) % 10);
    *p++ = '0' + ((year / 10) % 10);
    *p++ = '0' + (year % 10);
    *p++ = '\n';
    *p = '\0';
    
    return buf;
}

char *ctime(const time_t *timep)
{
    return asctime(localtime(timep));
}

/* Sleep functions */
unsigned int sleep(unsigned int seconds)
{
    struct timespec req;
    req.tv_sec  = (long)seconds;
    req.tv_nsec = 0;
    syscall(SYS_NANOSLEEP, (long)&req, 0, 0, 0, 0, 0);
    return 0;
}

int usleep(unsigned int usec)
{
    struct timespec req;
    req.tv_sec  = (long)(usec / 1000000);
    req.tv_nsec = (long)((usec % 1000000) * 1000);
    return (int)syscall(SYS_NANOSLEEP, (long)&req, 0, 0, 0, 0, 0);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req)
    {
        errno = EFAULT;
        return -1;
    }
    return (int)syscall(SYS_NANOSLEEP, (long)req, (long)rem, 0, 0, 0, 0);
}
