#ifndef AXIOME_PMM_H
#define AXIOME_PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096

void pmm_init(void);
void *pmm_alloc_frame(void);
void *pmm_alloc_frames(uint64_t count);
void *pmm_alloc_frames_below(uint64_t count, uint64_t max_phys);
void *pmm_alloc_frames_dma32(uint64_t count);
void *pmm_alloc_frames_high(uint64_t count, uint64_t max_phys);
void pmm_free_frame(void *addr);
void pmm_free_frames(void *addr, uint64_t count);
uint64_t pmm_free_count(void);
uint64_t pmm_total_frames(void);

#endif
