#ifndef AXIOME_LIBC_STDLIB_H
#define AXIOME_LIBC_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *p);
int atoi(const char *s);
long atol(const char *s);
void abort(void);
void exit(int status);
char *getenv(const char *name);

extern char **environ;

#endif
