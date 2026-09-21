#include "pmm.h"
#include "mmap.h"
#include "printk.h"

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define PAGE_SHIFT 12

extern uint8_t _bss_end[];

extern uint64_t mmap_max_addr;

static uint8_t *bitmap;
static uint64_t total_frames;
static uint64_t free_frames;
static uint64_t next_hint;

static inline void bm_set(uint64_t frame)
{
    bitmap[frame >> 3] |= (1 << (frame & 7));
}

static inline void bm_clear(uint64_t frame)
{
    bitmap[frame >> 3] &= ~(1 << (frame & 7));
}

static inline int bm_test(uint64_t frame)
{
    return (bitmap[frame >> 3] >> (frame & 7)) & 1;
}

static void mark_used(uint64_t base, uint64_t length)
{
    uint64_t start = (base + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t end = (base + length) >> PAGE_SHIFT;
    for (uint64_t i = start; i < end; i++)
    {
        if (!bm_test(i))
        {
            bm_set(i);
            free_frames--;
        }
    }
}

static void mark_free(uint64_t base, uint64_t length)
{
    uint64_t start = (base + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t end = (base + length) >> PAGE_SHIFT;
    for (uint64_t i = start; i < end; i++)
    {
        if (bm_test(i))
        {
            bm_clear(i);
            free_frames++;
        }
    }
}

void pmm_init(void)
{
    total_frames = (mmap_max_addr + PAGE_SIZE - 1) >> PAGE_SHIFT;

    uintptr_t bitmap_phys = (uintptr_t)_bss_end - KERNEL_VIRT_BASE;
    bitmap_phys = (bitmap_phys + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    bitmap = (uint8_t *)(bitmap_phys + KERNEL_VIRT_BASE);

    uint64_t bitmap_bytes = (total_frames + 7) >> 3;
    printk("PMM: max=0x%lx frames=%lu bitmap=%p (%lu bytes)\n",
           mmap_max_addr, total_frames, bitmap, bitmap_bytes);

    for (uint64_t i = 0; i < bitmap_bytes; i++)
        bitmap[i] = 0xFF;
    free_frames = 0;

    for (int i = 0; i < kernel_mmap.count; i++)
    {
        if (kernel_mmap.entries[i].type == 1)
            mark_free(kernel_mmap.entries[i].base, kernel_mmap.entries[i].length);
    }

    uintptr_t kernel_start = 0x200000;
    uintptr_t kernel_end = bitmap_phys + bitmap_bytes;
    mark_used(kernel_start, kernel_end - kernel_start);

    bm_set(0);
    free_frames--;

    for (uint64_t a = 0x800000; a < mmap_max_addr; a += 0x200000)
    {
        if (addr_in_reserved_region(a))
            mark_used(a, 0x200000);
    }

    next_hint = 0;
    printk("PMM: %lu free frames (%lu KB)\n", free_frames, free_frames << (PAGE_SHIFT - 10));
}

void *pmm_alloc_frame(void)
{
    uint64_t search_start = next_hint;
    for (uint64_t i = 0; i < total_frames; i++)
    {
        uint64_t idx = (search_start + i) % total_frames;
        if (!bm_test(idx))
        {
            bm_set(idx);
            free_frames--;
            next_hint = idx + 1;
            return (void *)(idx << PAGE_SHIFT);
        }
    }
    return 0;
}

void *pmm_alloc_frames(uint64_t count)
{
    if (count == 0)
        return 0;

    for (uint64_t start = 0; start + count <= total_frames; )
    {
        while (start < total_frames && bm_test(start))
            start++;
        if (start + count > total_frames)
            break;

        uint64_t free_run = 0;
        for (uint64_t j = start; j < total_frames && free_run < count; j++)
        {
            if (!bm_test(j))
                free_run++;
            else
                break;
        }

        if (free_run >= count)
        {
            for (uint64_t j = 0; j < count; j++)
                bm_set(start + j);
            free_frames -= count;
            return (void *)(start << PAGE_SHIFT);
        }

        start += free_run + 1;
    }
    return 0;
}

void *pmm_alloc_frames_below(uint64_t count, uint64_t max_phys)
{
    if (count == 0 || max_phys == 0)
        return 0;
    uint64_t max_frame = max_phys >> PAGE_SHIFT;
    if (max_frame == 0)
        return 0;
    if (max_frame > total_frames)
        max_frame = total_frames;
    if (count > max_frame)
        return 0;
    for (uint64_t start = 0; start + count <= max_frame; )
    {
        while (start < max_frame && bm_test(start))
            start++;
        if (start + count > max_frame)
            break;
        uint64_t free_run = 0;
        for (uint64_t j = start; j < max_frame && free_run < count; j++)
        {
            if (!bm_test(j))
                free_run++;
            else
                break;
        }
        if (free_run >= count)
        {
            for (uint64_t j = 0; j < count; j++)
                bm_set(start + j);
            free_frames -= count;
            return (void *)(start << PAGE_SHIFT);
        }
        start += free_run + 1;
    }
    return 0;
}

void *pmm_alloc_frames_dma32(uint64_t count)
{
    return pmm_alloc_frames_below(count, 0x100000000ULL);
}

void pmm_free_frame(void *addr)
{
    uint64_t frame = (uint64_t)addr >> PAGE_SHIFT;
    if (frame < total_frames && bm_test(frame))
    {
        bm_clear(frame);
        free_frames++;
    }
}

void pmm_free_frames(void *addr, uint64_t count)
{
    uint64_t start = (uint64_t)addr >> PAGE_SHIFT;
    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t f = start + i;
        if (f < total_frames && bm_test(f))
        {
            bm_clear(f);
            free_frames++;
        }
    }
}

uint64_t pmm_free_count(void)
{
    return free_frames;
}

uint64_t pmm_total_frames(void)
{
    return total_frames;
}
