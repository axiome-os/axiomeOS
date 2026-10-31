#ifndef AXIOME_LIBC_STDIO_H
#define AXIOME_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* FILE structure */
typedef struct {
    int fd;
    int flags;
    int eof;
    int err;
    char *buf;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_len;
} FILE;

/* Standard streams */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File modes */
#define _IOREAD  0x01
#define _IOWRITE 0x02
#define _IOBUF   0x04

/* EOF marker */
#define EOF (-1)

/* Seek origins */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Basic I/O */
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

int puts(const char *s);
int fputs(const char *s, FILE *stream);

int putchar(int c);
int fputc(int c, FILE *stream);

int getchar(void);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);

/* File operations */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);

#endif
