#include "stdlib.h"
#include "errno.h"
#include "syscall.h"
#include "string.h"
#include <stddef.h>

#define HEAP_SIZE (4UL * 1024 * 1024)
static unsigned char heap[HEAP_SIZE];
static int heap_init = 0;

char **environ;

/* Set by crt0 from the initial stack's envp vector before main() runs.
   Lives in libc (not the executable) so the executable has no copy
   relocation against this data symbol across the .sl boundary. */
void __axiome_set_environ(char **envp)
{
    environ = envp;
}

/* Simple free-list allocator with block headers */
typedef struct block {
    size_t size;        /* Size of user data (excluding header) */
    int free;           /* 1 if free, 0 if allocated */
    struct block *next; /* Next block in list */
} block_t;

#define BLOCK_SIZE sizeof(block_t)
#define ALIGN16(x) (((x) + 15UL) & ~15UL)

static block_t *free_list = NULL;

static void init_heap(void)
{
    if (heap_init)
        return;
    free_list = (block_t *)heap;
    free_list->size = HEAP_SIZE - BLOCK_SIZE;
    free_list->free = 1;
    free_list->next = NULL;
    heap_init = 1;
}

static void split_block(block_t *block, size_t size)
{
    if (block->size >= size + BLOCK_SIZE + 16)
    {
        block_t *new_block = (block_t *)((char *)block + BLOCK_SIZE + size);
        new_block->size = block->size - size - BLOCK_SIZE;
        new_block->free = 1;
        new_block->next = block->next;
        block->size = size;
        block->next = new_block;
    }
}

static void merge_free_blocks(void)
{
    block_t *curr = free_list;
    while (curr && curr->next)
    {
        if (curr->free && curr->next->free)
        {
            curr->size += BLOCK_SIZE + curr->next->size;
            curr->next = curr->next->next;
        }
        else
        {
            curr = curr->next;
        }
    }
}

void *malloc(size_t n)
{
    if (n == 0)
        return NULL;
    
    if (!heap_init)
        init_heap();
    
    size_t size = ALIGN16(n);
    block_t *curr = free_list;
    
    while (curr)
    {
        if (curr->free && curr->size >= size)
        {
            split_block(curr, size);
            curr->free = 0;
            return (void *)((char *)curr + BLOCK_SIZE);
        }
        curr = curr->next;
    }
    
    errno = ENOMEM;
    return NULL;
}

void free(void *p)
{
    if (!p)
        return;
    
    block_t *block = (block_t *)((char *)p - BLOCK_SIZE);
    block->free = 1;
    merge_free_blocks();
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;
    
    /* Check for overflow */
    if (nmemb > (size_t)-1 / size)
    {
        errno = ENOMEM;
        return NULL;
    }
    
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }
    
    block_t *block = (block_t *)((char *)ptr - BLOCK_SIZE);
    
    /* If the block is already large enough, return it */
    if (block->size >= size)
        return ptr;
    
    /* Allocate new block and copy data */
    void *new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;
    
    memcpy(new_ptr, ptr, block->size < size ? block->size : size);
    free(ptr);
    return new_ptr;
}

int atoi(const char *s)
{
    int neg = 0;
    int v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (*s == '-')
    {
        neg = 1;
        s++;
    }
    else if (*s == '+')
    {
        s++;
    }
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

long atol(const char *s)
{
    int neg = 0;
    long v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (*s == '-')
    {
        neg = 1;
        s++;
    }
    else if (*s == '+')
    {
        s++;
    }
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

void abort(void)
{
    syscall(SYS_EXIT, 134, 0, 0, 0, 0, 0);
    for (;;)
        __asm__ volatile ("hlt");
}

void exit(int status)
{
    syscall(SYS_EXIT, status, 0, 0, 0, 0, 0);
    for (;;)
        __asm__ volatile ("hlt");
}

char *getenv(const char *name)
{
    if (!name || !*name || !environ)
        return 0;
    size_t namelen = 0;
    while (name[namelen]) namelen++;
    for (char **p = environ; *p; p++)
    {
        const char *e = *p;
        size_t i = 0;
        while (i < namelen && e[i] == name[i]) i++;
        if (i == namelen && e[i] == '=')
            return (char *)(e + i + 1);
    }
    return 0;
}
