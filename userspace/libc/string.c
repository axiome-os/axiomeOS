#include "string.h"
#include "stdlib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        d[i] = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s)
    {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    }
    else
    {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
    {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
    {
        if (p[i] == ch)
            return (void *)(p + i);
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t i = 0;
    while (s[i])
        i++;
    return i;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i])
    {
        d[i] = src[i];
        i++;
    }
    d[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (const char *h = haystack; *h; h++)
    {
        const char *hn = h;
        const char *n = needle;
        while (*hn && *n && *hn == *n)
        {
            hn++;
            n++;
        }
        if (!*n)
            return (char *)h;
    }
    return 0;
}

char *strdup(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *new_str = malloc(len + 1);
    if (!new_str)
        return NULL;
    memcpy(new_str, s, len + 1);
    return new_str;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t i = 0;
    while (s[i])
    {
        const char *r = reject;
        int found = 0;
        while (*r)
        {
            if (s[i] == *r)
            {
                found = 1;
                break;
            }
            r++;
        }
        if (found)
            break;
        i++;
    }
    return i;
}

size_t strspn(const char *s, const char *accept)
{
    size_t i = 0;
    while (s[i])
    {
        const char *a = accept;
        int found = 0;
        while (*a)
        {
            if (s[i] == *a)
            {
                found = 1;
                break;
            }
            a++;
        }
        if (!found)
            break;
        i++;
    }
    return i;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s)
    {
        const char *a = accept;
        while (*a)
        {
            if (*s == *a)
                return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

static char *strtok_saveptr = NULL;

char *strtok(char *str, const char *delim)
{
    if (str)
        strtok_saveptr = str;
    
    if (!strtok_saveptr)
        return NULL;
    
    /* Skip leading delimiters */
    strtok_saveptr += strspn(strtok_saveptr, delim);
    
    if (*strtok_saveptr == '\0')
    {
        strtok_saveptr = NULL;
        return NULL;
    }
    
    /* Find end of token */
    char *token = strtok_saveptr;
    strtok_saveptr += strcspn(strtok_saveptr, delim);
    
    if (*strtok_saveptr != '\0')
    {
        *strtok_saveptr = '\0';
        strtok_saveptr++;
    }
    else
    {
        strtok_saveptr = NULL;
    }
    
    return token;
}
