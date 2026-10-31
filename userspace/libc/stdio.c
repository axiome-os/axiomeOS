#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include <stdarg.h>
#include <stddef.h>

#define OUTBUF_SIZE 256
static char outbuf[OUTBUF_SIZE];
static int outlen = 0;

/* Standard streams */
static FILE _stdin = {0, _IOREAD, 0, 0, NULL, 0, 0, 0};
static FILE _stdout = {1, _IOWRITE, 0, 0, NULL, 0, 0, 0};
static FILE _stderr = {2, _IOWRITE, 0, 0, NULL, 0, 0, 0};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

static void flush(void)
{
    if (outlen > 0)
    {
        write(1, outbuf, (size_t)outlen);
        outlen = 0;
    }
}

static void emit(char c)
{
    outbuf[outlen++] = c;
    if (outlen >= (int)sizeof(outbuf) - 1)
        flush();
}

static void emit_str(const char *s)
{
    while (*s)
        emit(*s++);
}

static void emit_uint_width(unsigned long v, int base, int upper, int width, int zero_pad)
{
    char tmp[24];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0)
    {
        tmp[i++] = '0';
    }
    else
    {
        while (v)
        {
            tmp[i++] = digits[v % (unsigned long)base];
            v /= (unsigned long)base;
        }
    }
    
    /* Padding */
    int pad_count = width - i;
    if (pad_count > 0)
    {
        char pad_char = zero_pad ? '0' : ' ';
        while (pad_count--)
            emit(pad_char);
    }
    
    while (i--)
        emit(tmp[i]);
}

static void emit_uint(unsigned long v, int base, int upper)
{
    emit_uint_width(v, base, upper, 0, 0);
}

static void emit_int_width(long v, int width, int zero_pad)
{
    if (v < 0)
    {
        emit('-');
        if (width > 0) width--;
        emit_uint_width((unsigned long)(-(v + 1)) + 1, 10, 0, width, zero_pad);
    }
    else
    {
        emit_uint_width((unsigned long)v, 10, 0, width, zero_pad);
    }
}

static void emit_int(long v)
{
    emit_int_width(v, 0, 0);
}

static void emit_str_width(const char *s, int width, int left_align)
{
    int len = 0;
    const char *p = s;
    while (*p++) len++;
    
    if (!left_align && width > len)
    {
        int pad = width - len;
        while (pad--)
            emit(' ');
    }
    
    emit_str(s);
    
    if (left_align && width > len)
    {
        int pad = width - len;
        while (pad--)
            emit(' ');
    }
}

int vsnprintf_internal(char *str, size_t size, const char *fmt, va_list ap, int do_write)
{
    size_t pos = 0;
    
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            if (do_write && str && pos < size - 1)
                str[pos] = *fmt;
            pos++;
            continue;
        }
        fmt++;
        if (*fmt == 0)
            break;
        if (*fmt == '%')
        {
            if (do_write && str && pos < size - 1)
                str[pos] = '%';
            pos++;
            continue;
        }

        /* Parse flags */
        int left_align = 0;
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0')
        {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        if (*fmt == 0) break;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
        {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 0) break;

        /* Parse precision (ignore for now) */
        if (*fmt == '.')
        {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                fmt++;
        }
        if (*fmt == 0) break;

        int longflag = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
        {
            longflag = 1;
            fmt++;
        }
        if (*fmt == 0)
            break;

        char spec = *fmt;
        char tmp_buf[64];
        const char *to_write = NULL;
        int tmp_len = 0;
        
        switch (spec)
        {
        case 'd':
        case 'i': {
            long v = longflag ? va_arg(ap, long) : (long)va_arg(ap, int);
            /* Manual formatting to tmp_buf */
            char *p = tmp_buf;
            if (v < 0) {
                *p++ = '-';
                v = -(v + 1) + 1;
            }
            char digits[24];
            int i = 0;
            unsigned long uv = (unsigned long)v;
            if (uv == 0) digits[i++] = '0';
            else {
                while (uv) {
                    digits[i++] = '0' + (uv % 10);
                    uv /= 10;
                }
            }
            while (i--) *p++ = digits[i];
            *p = 0;
            to_write = tmp_buf;
            tmp_len = p - tmp_buf;
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long v = longflag ? va_arg(ap, unsigned long)
                                       : (unsigned long)va_arg(ap, unsigned int);
            int base = (spec == 'u') ? 10 : 16;
            const char *digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char *p = tmp_buf;
            char dbuf[24];
            int i = 0;
            if (v == 0) dbuf[i++] = '0';
            else {
                while (v) {
                    dbuf[i++] = digits[v % base];
                    v /= base;
                }
            }
            while (i--) *p++ = dbuf[i];
            *p = 0;
            to_write = tmp_buf;
            tmp_len = p - tmp_buf;
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            unsigned long v = (unsigned long)(unsigned long long)ptr;
            char *p = tmp_buf;
            *p++ = '0';
            *p++ = 'x';
            const char *digits = "0123456789abcdef";
            char dbuf[24];
            int i = 0;
            if (v == 0) dbuf[i++] = '0';
            else {
                while (v) {
                    dbuf[i++] = digits[v % 16];
                    v /= 16;
                }
            }
            while (i--) *p++ = dbuf[i];
            *p = 0;
            to_write = tmp_buf;
            tmp_len = p - tmp_buf;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            to_write = s;
            const char *p = s;
            while (*p++) tmp_len++;
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            tmp_buf[0] = (char)c;
            tmp_buf[1] = 0;
            to_write = tmp_buf;
            tmp_len = 1;
            break;
        }
        default:
            tmp_buf[0] = '%';
            tmp_buf[1] = spec;
            tmp_buf[2] = 0;
            to_write = tmp_buf;
            tmp_len = 2;
            break;
        }
        
        /* Apply width padding */
        if (!left_align && width > tmp_len)
        {
            int pad = width - tmp_len;
            while (pad--)
            {
                if (do_write && str && pos < size - 1)
                    str[pos] = zero_pad ? '0' : ' ';
                pos++;
            }
        }
        
        /* Write content */
        for (int i = 0; i < tmp_len; i++)
        {
            if (do_write && str && pos < size - 1)
                str[pos] = to_write[i];
            pos++;
        }
        
        /* Right padding */
        if (left_align && width > tmp_len)
        {
            int pad = width - tmp_len;
            while (pad--)
            {
                if (do_write && str && pos < size - 1)
                    str[pos] = ' ';
                pos++;
            }
        }
    }

    if (do_write && str && size > 0)
        str[pos < size - 1 ? pos : size - 1] = 0;
    
    return (int)pos;
}

int vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            emit(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == 0)
            break;
        if (*fmt == '%')
        {
            emit('%');
            continue;
        }

        /* Parse flags */
        int left_align = 0;
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0')
        {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        if (*fmt == 0) break;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
        {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 0) break;

        /* Parse precision (ignore for now) */
        if (*fmt == '.')
        {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                fmt++;
        }
        if (*fmt == 0) break;

        int longflag = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
        {
            longflag = 1;
            fmt++;
        }
        if (*fmt == 0)
            break;

        char spec = *fmt;
        switch (spec)
        {
        case 'd':
        case 'i': {
            long v = longflag ? va_arg(ap2, long) : (long)va_arg(ap2, int);
            emit_int_width(v, width, zero_pad);
            break;
        }
        case 'u': {
            unsigned long v = longflag ? va_arg(ap2, unsigned long)
                                       : (unsigned long)va_arg(ap2, unsigned int);
            emit_uint_width(v, 10, 0, width, zero_pad);
            break;
        }
        case 'x': {
            unsigned long v = longflag ? va_arg(ap2, unsigned long)
                                       : (unsigned long)va_arg(ap2, unsigned int);
            emit_uint_width(v, 16, 0, width, zero_pad);
            break;
        }
        case 'X': {
            unsigned long v = longflag ? va_arg(ap2, unsigned long)
                                       : (unsigned long)va_arg(ap2, unsigned int);
            emit_uint_width(v, 16, 1, width, zero_pad);
            break;
        }
        case 'p': {
            void *p = va_arg(ap2, void *);
            emit_str("0x");
            emit_uint((unsigned long)(unsigned long long)p, 16, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap2, const char *);
            if (!s)
                s = "(null)";
            emit_str_width(s, width, left_align);
            break;
        }
        case 'c': {
            int c = va_arg(ap2, int);
            emit((char)c);
            break;
        }
        default:
            emit('%');
            emit(spec);
            break;
        }
    }

    flush();
    va_end(ap2);
    return 0;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    /* Simple implementation: format to a temporary buffer and write */
    char buf[1024];
    int n = vsnprintf_internal(buf, sizeof(buf), fmt, ap, 1);
    if (n > 0)
        fwrite(buf, 1, n, stream);
    return n;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf_internal(str, (size_t)-1, fmt, ap, 1);
    va_end(ap);
    return r;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf_internal(str, size, fmt, ap, 1);
    va_end(ap);
    return r;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    return vsnprintf_internal(str, size, fmt, ap, 1);
}

int puts(const char *s)
{
    emit_str(s);
    emit('\n');
    flush();
    return 0;
}

int fputs(const char *s, FILE *stream)
{
    return fwrite(s, 1, strlen(s), stream);
}

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, stream) == 1 ? c : EOF;
}

int getchar(void)
{
    char c;
    if (read(0, &c, 1) > 0)
        return (unsigned char)c;
    return -1;
}

int fgetc(FILE *stream)
{
    unsigned char c;
    if (fread(&c, 1, 1, stream) == 1)
        return c;
    return EOF;
}

char *fgets(char *s, int size, FILE *stream)
{
    if (size <= 0)
        return NULL;
    
    int i;
    for (i = 0; i < size - 1; i++)
    {
        int c = fgetc(stream);
        if (c == EOF)
        {
            if (i == 0)
                return NULL;
            break;
        }
        s[i] = (char)c;
        if (c == '\n')
        {
            i++;
            break;
        }
    }
    s[i] = '\0';
    return s;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    int file_flags = 0;
    
    if (!mode || !*mode)
        return NULL;
    
    switch (mode[0])
    {
    case 'r':
        flags = _IOREAD;
        file_flags = O_RDONLY;
        break;
    case 'w':
        flags = _IOWRITE;
        file_flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case 'a':
        flags = _IOWRITE;
        file_flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    default:
        return NULL;
    }
    
    /* Check for '+' modifier */
    if (mode[1] == '+')
    {
        flags = _IOREAD | _IOWRITE;
        if (mode[0] == 'r')
            file_flags = O_RDWR;
        else if (mode[0] == 'w')
            file_flags = O_RDWR | O_CREAT | O_TRUNC;
        else
            file_flags = O_RDWR | O_CREAT | O_APPEND;
    }
    
    int fd = open(path, file_flags);
    if (fd < 0)
        return NULL;
    
    FILE *f = malloc(sizeof(FILE));
    if (!f)
    {
        close(fd);
        return NULL;
    }
    
    f->fd = fd;
    f->flags = flags;
    f->eof = 0;
    f->err = 0;
    f->buf = NULL;
    f->buf_size = 0;
    f->buf_pos = 0;
    f->buf_len = 0;
    
    return f;
}

int fclose(FILE *stream)
{
    if (!stream)
        return EOF;
    
    if (stream == stdin || stream == stdout || stream == stderr)
        return 0;
    
    fflush(stream);
    int r = close(stream->fd);
    if (stream->buf)
        free(stream->buf);
    free(stream);
    return r < 0 ? EOF : 0;
}

int fflush(FILE *stream)
{
    if (!stream)
        return 0;
    
    /* For now, no buffering, so nothing to flush */
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || !ptr || size == 0 || nmemb == 0)
        return 0;
    
    size_t total = size * nmemb;
    long n = read(stream->fd, ptr, total);
    
    if (n < 0)
    {
        stream->err = 1;
        return 0;
    }
    
    if (n == 0)
        stream->eof = 1;
    
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || !ptr || size == 0 || nmemb == 0)
        return 0;
    
    size_t total = size * nmemb;
    long n = write(stream->fd, ptr, total);
    
    if (n < 0)
    {
        stream->err = 1;
        return 0;
    }
    
    return (size_t)n / size;
}

int feof(FILE *stream)
{
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream)
{
    return stream ? stream->err : 0;
}

void clearerr(FILE *stream)
{
    if (stream)
    {
        stream->eof = 0;
        stream->err = 0;
    }
}
