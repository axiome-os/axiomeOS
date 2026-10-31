#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "util.h"

static const char *const mon_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const mon_full[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const day_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const day_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

/* Emit an unsigned value in decimal, zero- or space-padded to `digits`. */
static void emit_pad(unsigned long v, int digits, int zero)
{
    char tb[32];
    int n = 0;
    if (v == 0)
        tb[n++] = '0';
    while (v > 0)
    {
        tb[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n < digits)
    {
        putchar(zero ? '0' : ' ');
        n++;
    }
    while (n > 0)
        putchar(tb[--n]);
}

static int iso_week(const struct tm *t)
{
    /* Monday-based week number. */
    int monday = (t->tm_wday + 6) % 7;
    return (t->tm_yday + 7 - monday) / 7;
}

static void print_fmt(const char *fmt, const struct tm *t, time_t epoch)
{
    for (const char *p = fmt; *p; p++)
    {
        if (*p != '%')
        {
            putchar(*p);
            continue;
        }
        p++;
        char c = *p;
        int h;
        int anyx = (t->tm_hour >= 12) ? 1 : 0;
        switch (c)
        {
        case 0:
            putchar('%');
            return;
        case '%':
            putchar('%');
            break;
        case 'a': printf("%s", day_abbr[t->tm_wday]); break;
        case 'A': printf("%s", day_full[t->tm_wday]); break;
        case 'b':
        case 'h': printf("%s", mon_abbr[t->tm_mon]); break;
        case 'B': printf("%s", mon_full[t->tm_mon]); break;
        case 'c':
            print_fmt("%a %b %e %H:%M:%S %Y", t, epoch);
            break;
        case 'C': emit_pad((unsigned long)((t->tm_year + 1900) / 100), 2, 1); break;
        case 'd': emit_pad((unsigned long)t->tm_mday, 2, 1); break;
        case 'D': print_fmt("%m/%d/%y", t, epoch); break;
        case 'e': emit_pad((unsigned long)t->tm_mday, 2, 0); break;
        case 'F': print_fmt("%Y-%m-%d", t, epoch); break;
        case 'g': emit_pad((unsigned long)((t->tm_year + 1900) % 100), 2, 1); break;
        case 'G': emit_pad((unsigned long)(t->tm_year + 1900), 4, 1); break;
        case 'H': emit_pad((unsigned long)t->tm_hour, 2, 1); break;
        case 'I':
            h = t->tm_hour % 12;
            if (h == 0) h = 12;
            emit_pad((unsigned long)h, 2, 1);
            break;
        case 'j': emit_pad((unsigned long)(t->tm_yday + 1), 3, 1); break;
        case 'k':
            emit_pad((unsigned long)t->tm_hour, 2, (t->tm_hour < 10) ? 0 : 1);
            break;
        case 'l':
            h = t->tm_hour % 12;
            if (h == 0) h = 12;
            emit_pad((unsigned long)h, 2, (h < 10) ? 0 : 1);
            break;
        case 'm': emit_pad((unsigned long)(t->tm_mon + 1), 2, 1); break;
        case 'M': emit_pad((unsigned long)t->tm_min, 2, 1); break;
        case 'n': putchar('\n'); break;
        case 'N': emit_pad(0, 9, 1); break;
        case 'p': printf("%s", anyx ? "PM" : "AM"); break;
        case 'P': printf("%s", anyx ? "pm" : "am"); break;
        case 'r': print_fmt("%I:%M:%S %p", t, epoch); break;
        case 'R': print_fmt("%H:%M", t, epoch); break;
        case 's': emit_pad((unsigned long)epoch, 0, 0); break;
        case 'S': emit_pad((unsigned long)t->tm_sec, 2, 1); break;
        case 't': putchar('\t'); break;
        case 'T': print_fmt("%H:%M:%S", t, epoch); break;
        case 'u':
            emit_pad((unsigned long)((t->tm_wday == 0) ? 7 : t->tm_wday), 1, 1);
            break;
        case 'U': emit_pad((unsigned long)((t->tm_yday + 7 - t->tm_wday) / 7), 2, 1); break;
        case 'V': emit_pad((unsigned long)iso_week(t), 2, 1); break;
        case 'w': emit_pad((unsigned long)t->tm_wday, 1, 1); break;
        case 'W':
            emit_pad((unsigned long)((t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7),
                     2, 1);
            break;
        case 'x': print_fmt("%m/%d/%y", t, epoch); break;
        case 'X': print_fmt("%H:%M:%S", t, epoch); break;
        case 'y': emit_pad((unsigned long)((t->tm_year + 1900) % 100), 2, 1); break;
        case 'Y': emit_pad((unsigned long)(t->tm_year + 1900), 4, 1); break;
        case 'z': printf("+0000"); break;
        case 'Z': printf("UTC"); break;
        case ':':
            if (p[1] == 'z')
            {
                printf("+00:00");
                p++;
            }
            else
            {
                putchar(':');
            }
            break;
        default:
            putchar('%');
            putchar(c);
            break;
        }
    }
}

static void usage(void)
{
    puts("Usage: date [OPTION]... [+FORMAT]");
    puts("Display the current time in the given FORMAT.");
    puts("");
    puts("  -u, --utc, --universal   print Coordinated Universal Time");
    puts("  -R, --rfc-email          output in RFC 2822 format");
    puts("  -I[TIMESPEC], --iso-8601[=TIMESPEC]");
    puts("                          output in ISO 8601 (TIMESPEC: date,");
    puts("                          hours, minutes, seconds (default), ns)");
    puts("      --rfc-3339=TIMESPEC  output in RFC 3339 format (TIMESPEC:");
    puts("                          date, seconds (default), ns)");
    puts("      --help               display this help and exit");
    puts("");
    puts("FORMAT characters include: %a %b %e %H:%M:%S %Z %Y %s and more.");
}

int main(int argc, char **argv)
{
    int utc = 0;
    int rfc_email = 0;
    const char *iso = 0;       /* "-I" style: use default seconds */
    const char *rfc3339 = 0;
    const char *format = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0)
        {
            for (int j = i + 1; j < argc; j++)
            {
                if (argv[j][0] == '+' && argv[j][1] != 0 && !format)
                    format = argv[j];
                else
                {
                    fprintf(stderr, "date: extra operand '%s'\n", argv[j]);
                    sys_exit(1);
                }
            }
            break;
        }
        if (a[0] != '-' || a[1] == 0)
        {
            if (a[0] == '+' && a[1] != 0 && !format)
            {
                format = a;
                continue;
            }
            fprintf(stderr, "date: extra operand '%s'\n", a);
            sys_exit(1);
        }
        if (a[1] == '-')
        {
            const char *opt = a + 2;
            if (strcmp(opt, "help") == 0) { usage(); sys_exit(0); }
            if (strcmp(opt, "version") == 0)
            {
                puts("date (axiomeOS) 1.0");
                sys_exit(0);
            }
            if (strcmp(opt, "utc") == 0 ||
                strcmp(opt, "universal") == 0) { utc = 1; continue; }
            if (strcmp(opt, "rfc-email") == 0) { rfc_email = 1; continue; }
            if (strcmp(opt, "iso-8601") == 0) { iso = "seconds"; continue; }
            if (strncmp(opt, "iso-8601=", 9) == 0) { iso = opt + 9; continue; }
            if (strncmp(opt, "rfc-3339=", 9) == 0) { rfc3339 = opt + 9; continue; }
            if (strcmp(opt, "date") == 0)
            {
                fprintf(stderr, "date: -d/--date is not supported\n");
                sys_exit(1);
            }
            if (strcmp(opt, "reference") == 0)
            {
                fprintf(stderr, "date: -r/--reference requires file "
                        "timestamps, which this VFS stat does not provide\n");
                sys_exit(1);
            }
            fprintf(stderr, "date: unrecognized option '%s'\n", a);
            sys_exit(1);
        }
        for (const char *c = a + 1; *c; c++)
        {
            switch (*c)
            {
            case 'u': utc = 1; break;
            case 'R': rfc_email = 1; break;
            case 'I':
                if (c[1] == '=')
                    iso = c + 2;
                else if (c[1] != 0)
                    iso = c + 1;
                else
                    iso = "seconds";
                while (*c)
                    c++;
                c--;
                break;
            case 'd':
                fprintf(stderr, "date: -d is not supported\n");
                sys_exit(1);
                break;
            case 'r':
                fprintf(stderr, "date: -r requires file timestamps, which "
                        "this VFS stat does not provide\n");
                sys_exit(1);
                break;
            case 's':
                fprintf(stderr, "date: -s (set time) is not supported\n");
                sys_exit(1);
                break;
            default:
                fprintf(stderr, "date: invalid option -- '%c'\n", *c);
                fprintf(stderr, "Try 'date --help' for more information.\n");
                sys_exit(1);
            }
        }
    }

    time_t now = time(NULL);
    const struct tm *t = utc ? gmtime(&now) : localtime(&now);

    if (!format)
    {
        if (rfc_email)
            format = "%a, %d %b %Y %H:%M:%S %z";
        else if (iso)
        {
            if (strcmp(iso, "date") == 0)
                format = "%Y-%m-%d";
            else if (strcmp(iso, "hours") == 0)
                format = "%Y-%m-%dT%H%:z";
            else if (strcmp(iso, "minutes") == 0)
                format = "%Y-%m-%dT%H:%M%:z";
            else if (strcmp(iso, "ns") == 0)
                format = "%Y-%m-%dT%H:%M:%S,%N%:z";
            else
                format = "%Y-%m-%dT%H:%M:%S%:z";
        }
        else if (rfc3339)
        {
            if (strcmp(rfc3339, "date") == 0)
                format = "%Y-%m-%d";
            else if (strcmp(rfc3339, "ns") == 0)
                format = "%Y-%m-%d %H:%M:%S,%N%:z";
            else
                format = "%Y-%m-%d %H:%M:%S%:z";
        }
        else
            format = "%a %b %e %H:%M:%S %Z %Y";
    }

    print_fmt(format, t, now);
    putchar('\n');
    sys_exit(0);
    return 0;
}