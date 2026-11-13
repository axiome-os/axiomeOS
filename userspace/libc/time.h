#ifndef AXIOME_LIBC_TIME_H
#define AXIOME_LIBC_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long suseconds_t;

/* A host build (the libc unit tests compile this header with the system
   preprocessor) may already have pulled in the platform's definitions via
   <sys/types.h>. Those guards are BSD/macOS spellings; they are never set in
   the freestanding build, where the definitions below are required. */
#ifndef _STRUCT_TIMEVAL
struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};
#endif

#ifndef _STRUCT_TIMESPEC
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* Time functions */
time_t time(time_t *tloc);
int gettimeofday(struct timeval *tv, void *tz);
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
char *asctime(const struct tm *tm);
char *ctime(const time_t *timep);

/* Sleep functions */
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
