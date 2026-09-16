/* x86_64 MMU: 4-level paging (PML4/PDP/PD/PT). Implements the portable
   arch_mmu.h interface. The root table is identity-mapped, so physical
   addresses of page tables double as kernel virtual addresses. */

#include <stddef.h>

#include "arch_mmu.h"
#include "mmu.h"
#include "pmm.h"
#include "mmap.h"
#include "printk.h"

extern uint64_t mmap_max_addr;
extern uint64_t pd_table[];
extern uint64_t pd_table2[];
extern uint64_t pd_table3[];

struct mmu_root {
    uint64_t entry[512];
};

static struct mmu_root *kernel_root;

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline void flush_tlb(void)
{
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
}

static inline void __attribute__((unused)) invlpg(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t pat_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void pat_wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr) : "memory");
}

static inline void pat_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

/* PAT slot currently used for Write-Combining. Defaults to PA4 (binary 100:
   PAT=1, PCD=0, PWT=0), the conventional UEFI WC slot. pat_init() verifies
   IA32_PAT and relocates this if firmware placed WC elsewhere. */
static int pat_wc_index = PAT_WC_INDEX;
static int pat_initialized = 0;
static int pat_available = 0;

int mmu_pat_wc_index(void)
{
    return pat_wc_index;
}

int mmu_pat_has_wc(void)
{
    return pat_available;
}

/* Step 1 of WC enablement: make sure IA32_PAT (0x277) has a 01h (WC) entry.
   UEFI firmware normally already programs PA4=WC; if no slot holds WC we
   program PA4 ourselves, preserving the other seven slots. Idempotent so it
   can run both before the early GOP mapping (which happens in axboot_parse,
   ahead of vmm_init) and again from mmu_arch_init(). */
void mmu_pat_init(void)
{
    if (pat_initialized)
        return;
    pat_initialized = 1;

    uint32_t eax, ebx, ecx, edx;
    pat_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1u << 16)))
    {
        klog("PAT: CPUID reports no PAT support, WC unavailable\n");
        return;
    }

    uint64_t pat = pat_rdmsr(IA32_PAT_MSR);
    klog("PAT: initial=0x%lx\n", (unsigned long)pat);

    for (int i = 0; i < 8; i++)
    {
        uint8_t t = (pat >> (i * 8)) & 0xFF;
        if (t == PAT_WC)
        {
            pat_wc_index = i;
            pat_available = 1;
            klog("PAT: WC already at PA%d\n", i);
            return;
        }
    }

    /* No WC slot: install WC at PA4, keep PA0..PA3,PA5..PA7 as-is. */
    pat &= ~(0xFFULL << (PAT_WC_INDEX * 8));
    pat |= ((uint64_t)PAT_WC << (PAT_WC_INDEX * 8));
    __asm__ volatile("wbinvd" ::: "memory");
    pat_wrmsr(IA32_PAT_MSR, pat);
    flush_tlb();
    pat_wc_index = PAT_WC_INDEX;
    pat_available = 1;
    klog("PAT: programmed WC at PA%d pat=0x%lx\n",
           PAT_WC_INDEX, (unsigned long)pat_rdmsr(IA32_PAT_MSR));
}

static uint64_t to_x86_flags(uint32_t flags)
{
    uint64_t x = X86_PTE_PRESENT;
    if (flags & MMU_WRITE)    x |= X86_PTE_WRITE;
    if (flags & MMU_USER)     x |= X86_PTE_USER;
    if (flags & MMU_HUGE)     x |= X86_PTE_HUGE;
    if (flags & MMU_GLOBAL)   x |= X86_PTE_GLOBAL;
    /* Step 2 of WC enablement: PAT index 4 (100b) selects WC.
       4 KiB PTE: PAT=bit7, PCD=0, PWT=0.
       2 MiB PDE: PAT=bit12 (+PS bit7 from MMU_HUGE), PCD=0, PWT=0.
       MMU_UNCACHED (PWT|PCD, PAT=0 -> index 3 = UC) may combine with WC;
       that yields index 7, which firmware leaves as UC -- a safe fallback
       for contradictory requests. Without PAT support WC degrades to UC. */
    if (flags & MMU_WC)
    {
        if (pat_available || !pat_initialized)
        {
            if (flags & MMU_HUGE)
                x |= X86_PTE_PAT_LARGE;
            else
                x |= X86_PTE_PAT;
        }
        else
        {
            x |= X86_PTE_PWT | X86_PTE_PCD;
        }
    }
    if (flags & MMU_UNCACHED) x |= X86_PTE_PWT | X86_PTE_PCD;
    if (flags & MMU_NX)       x |= X86_PTE_NX;
    if (flags & MMU_SHARED)   x |= X86_PTE_SHARED;
    return x;
}

static int split_huge(uint64_t *entry, uint64_t base)
{
    (void)base;
    uint64_t old = *entry;
    if (!(old & X86_PTE_PRESENT))
        return -1;
    if (!(old & X86_PTE_HUGE))
        return 0;

    uint64_t page = (uint64_t)pmm_alloc_frame();
    if (!page)
        return -1;

    uint64_t *pt = (uint64_t *)(uintptr_t)page;
    uint64_t phys = old & X86_PTE_ADDR_MASK;
    /* Preserve PWT/PCD (bits 3-4) and translate a large-page WC request
       (PAT_LARGE, bit 12) into the small-page PAT encoding (bit 7).
       Bit 7 of the old entry is PS, not PAT, so clear it first. */
    int was_wc_large = (old & X86_PTE_PAT_LARGE) != 0;
    uint64_t flags = old & 0x1FF;
    flags &= ~X86_PTE_HUGE;
    flags &= ~X86_PTE_GLOBAL;
    if (was_wc_large)
        flags |= X86_PTE_PAT;
    if (old & X86_PTE_NX)
        flags |= X86_PTE_NX;

    for (int i = 0; i < 512; i++)
        pt[i] = (phys + i * PAGE_SIZE) | flags;

    *entry = page | (old & 0x1FF & ~X86_PTE_HUGE);
    flush_tlb();
    return 0;
}

static uint64_t *walk_page(struct mmu_root *root, uint64_t virt, int alloc)
{
    if (!root)
        root = kernel_root;

    uint64_t idx4 = (virt >> 39) & 0x1FF;
    uint64_t idx3 = (virt >> 30) & 0x1FF;
    uint64_t idx2 = (virt >> 21) & 0x1FF;
    uint64_t idx1 = (virt >> 12) & 0x1FF;

    uint64_t *entry = &root->entry[idx4];
    if (!(*entry & X86_PTE_PRESENT))
    {
        if (!alloc) return 0;
        uint64_t page = (uint64_t)pmm_alloc_frame();
        if (!page) return 0;
        uint64_t *zero = (uint64_t *)(uintptr_t)page;
        for (int i = 0; i < 512; i++)
            zero[i] = 0;
        *entry = page | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_USER;
    }

    uint64_t pdp_phys = *entry & X86_PTE_ADDR_MASK;
    uint64_t *pdp = (uint64_t *)(uintptr_t)pdp_phys;
    entry = &pdp[idx3];

    if (*entry & X86_PTE_HUGE)
    {
        uint64_t base = virt & ~((1ULL << 30) - 1);
        if (split_huge(entry, base) < 0)
            return 0;
    }

    if (!(*entry & X86_PTE_PRESENT))
    {
        if (!alloc) return 0;
        uint64_t page = (uint64_t)pmm_alloc_frame();
        if (!page) return 0;
        uint64_t *zero = (uint64_t *)(uintptr_t)page;
        for (int i = 0; i < 512; i++)
            zero[i] = 0;
        *entry = page | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_USER;
    }

    uint64_t pd_phys = *entry & X86_PTE_ADDR_MASK;
    uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;
    entry = &pd[idx2];

    if (*entry & X86_PTE_HUGE)
    {
        uint64_t base = virt & ~((1ULL << 21) - 1);
        if (split_huge(entry, base) < 0)
            return 0;
    }

    if (!(*entry & X86_PTE_PRESENT))
    {
        if (!alloc) return 0;
        uint64_t page = (uint64_t)pmm_alloc_frame();
        if (!page) return 0;
        uint64_t *zero = (uint64_t *)(uintptr_t)page;
        for (int i = 0; i < 512; i++)
            zero[i] = 0;
        *entry = page | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_USER;
    }

    uint64_t pt_phys = *entry & X86_PTE_ADDR_MASK;
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;
    return &pt[idx1];
}

struct mmu_root *mmu_current_root(void)
{
    return (struct mmu_root *)(uintptr_t)(read_cr3() & X86_PTE_ADDR_MASK);
}

struct mmu_root *mmu_kernel_root(void)
{
    return kernel_root;
}

void mmu_arch_init(void)
{
    /* Program PAT before any WC mapping is created. */
    mmu_pat_init();
    kernel_root = (struct mmu_root *)(uintptr_t)read_cr3();

    uint64_t *pd = pd_table;
    for (uint64_t addr = 0x800000; addr < mmap_max_addr; addr += 0x200000)
    {
        int idx = addr >> 21;
        if (idx >= 512) break;
        if (pd[idx] & X86_PTE_PRESENT) continue;
        if (addr_in_reserved_region(addr))
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_HUGE;
        else
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_HUGE;
    }
    flush_tlb();

    pd = pd_table2;
    for (int idx = 0; idx < 512; idx++)
    {
        if (pd[idx] & X86_PTE_PRESENT) continue;
        uint64_t addr = 0x80000000ULL + idx * 0x200000ULL;
        if (addr_in_reserved_region(addr))
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_HUGE;
        else
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_HUGE;
    }

    pd = pd_table3;
    for (int idx = 0; idx < 512; idx++)
    {
        if (pd[idx] & X86_PTE_PRESENT) continue;
        uint64_t addr = 0xC0000000ULL + idx * 0x200000ULL;
        if (addr_in_reserved_region(addr))
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_HUGE;
        else
            pd[idx] = addr | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_HUGE;
    }

    flush_tlb();

    uint64_t pdp_page = (uint64_t)pmm_alloc_frame();
    if (pdp_page)
    {
        uint64_t *pdp = (uint64_t *)(uintptr_t)pdp_page;
        for (int i = 0; i < 512; i++)
            pdp[i] = 0;
        kernel_root->entry[508] = pdp_page | X86_PTE_PRESENT | X86_PTE_WRITE;
        flush_tlb();
    }
}

int mmu_map(struct mmu_root *root, uint64_t virt, uint64_t phys, uint32_t flags)
{
    if (!root)
        root = kernel_root;
    uint64_t *pte = walk_page(root, virt, 1);
    if (!pte) return -1;
    if (*pte & X86_PTE_PRESENT)
        return -1;
    *pte = (phys & X86_PTE_ADDR_MASK) | to_x86_flags(flags);
    if (root == kernel_root)
        flush_tlb();
    return 0;
}

int mmu_unmap(struct mmu_root *root, uint64_t virt)
{
    if (!root)
        root = kernel_root;
    uint64_t *pte = walk_page(root, virt, 0);
    if (!pte || !(*pte & X86_PTE_PRESENT))
        return -1;
    *pte = 0;
    if (root == kernel_root)
        flush_tlb();
    return 0;
}

uint64_t mmu_virt_to_phys(struct mmu_root *root, uint64_t virt)
{
    if (!root)
        root = kernel_root;
    uint64_t *pte = walk_page(root, virt, 0);
    if (!pte || !(*pte & X86_PTE_PRESENT))
        return 0;
    return (*pte & X86_PTE_ADDR_MASK) | (virt & 0xFFF);
}

/* Non-allocating page-table walk: returns the PTE flags covering `virt`, or 0
   if any level is not present. Huge pages are accepted and reported as-is. */
static uint64_t range_pte(struct mmu_root *root, uint64_t virt)
{
    uint64_t idx4 = (virt >> 39) & 0x1FF;
    uint64_t *entry = &root->entry[idx4];
    if (!(*entry & X86_PTE_PRESENT))
        return 0;
    if (*entry & X86_PTE_HUGE)
        return *entry;

    uint64_t *pdp = (uint64_t *)(uintptr_t)(*entry & X86_PTE_ADDR_MASK);
    entry = &pdp[(virt >> 30) & 0x1FF];
    if (!(*entry & X86_PTE_PRESENT))
        return 0;
    if (*entry & X86_PTE_HUGE)
        return *entry;

    uint64_t *pd = (uint64_t *)(uintptr_t)(*entry & X86_PTE_ADDR_MASK);
    entry = &pd[(virt >> 21) & 0x1FF];
    if (!(*entry & X86_PTE_PRESENT))
        return 0;
    if (*entry & X86_PTE_HUGE)
        return *entry;

    uint64_t *pt = (uint64_t *)(uintptr_t)(*entry & X86_PTE_ADDR_MASK);
    entry = &pt[(virt >> 12) & 0x1FF];
    if (!(*entry & X86_PTE_PRESENT))
        return 0;
    return *entry;
}

int mmu_check_user_range(struct mmu_root *root, uint64_t virt, size_t len,
                         int write)
{
    if (!root)
        root = kernel_root;
    if (len == 0)
        return 1;
    if (virt > UINT64_MAX - len)
        return 0;

    uint64_t end = virt + len;
    while (virt < end)
    {
        uint64_t flags = range_pte(root, virt);
        if (!(flags & X86_PTE_PRESENT) || !(flags & X86_PTE_USER))
            return 0;
        if (write && !(flags & X86_PTE_WRITE))
            return 0;
        uint64_t step = PAGE_SIZE - (virt & (PAGE_SIZE - 1));
        uint64_t remain = end - virt;
        if (step > remain)
            step = remain;
        virt += step;
    }
    return 1;
}

int mmu_protect(struct mmu_root *root, uint64_t virt, uint32_t flags)
{
    if (!root)
        root = kernel_root;
    uint64_t *pte = walk_page(root, virt, 0);
    if (!pte || !(*pte & X86_PTE_PRESENT))
        return -1;
    uint64_t phys = *pte & X86_PTE_ADDR_MASK;
    *pte = phys | to_x86_flags(flags);
    /* Always invalidate: when the target root is the active one (e.g. the ELF
       loader hardening .text while the new address space is live), stale TLB
       entries must not keep granting the old permission bits. */
    flush_tlb();
    return 0;
}

static struct mmu_root *clone_level(struct mmu_root *src, int level, int deep_user)
{
    struct mmu_root *dst = (struct mmu_root *)(uintptr_t)pmm_alloc_frame();
    if (!dst)
        return 0;
    for (int i = 0; i < 512; i++)
        dst->entry[i] = 0;

    for (int i = 0; i < 512; i++)
    {
        uint64_t e = src->entry[i];
        if (!(e & X86_PTE_PRESENT))
            continue;

        int is_user = (e & X86_PTE_USER);

        if (!is_user)
        {
            dst->entry[i] = e;
            continue;
        }

        if (!deep_user)
        {
            dst->entry[i] = 0;
            continue;
        }

        if (level > 1)
        {
            if ((level == 2) && (e & X86_PTE_HUGE))
            {
                dst->entry[i] = e;
                continue;
            }
            uint64_t *next_src = (uint64_t *)(uintptr_t)(e & X86_PTE_ADDR_MASK);
            uint64_t *next_dst = (uint64_t *)(uintptr_t)clone_level(
                (struct mmu_root *)next_src, level - 1, deep_user);
            if (!next_dst)
                return 0;
            dst->entry[i] = (uint64_t)(uintptr_t)next_dst | (e & 0x1FF & ~X86_PTE_HUGE);
        }
        else
        {
            uint64_t *next_src = (uint64_t *)(uintptr_t)(e & X86_PTE_ADDR_MASK);
            uint64_t np = (uint64_t)pmm_alloc_frame();
            if (!np)
                return 0;
            __builtin_memcpy((void *)(uintptr_t)np, next_src, PAGE_SIZE);
            dst->entry[i] = np | (e & 0x1FF);
        }
    }
    return dst;
}

struct mmu_root *mmu_new_user_root(void)
{
    return clone_level(kernel_root, 4, 0);
}

struct mmu_root *mmu_clone_root(struct mmu_root *src)
{
    return clone_level(src, 4, 1);
}

static void free_level(uint64_t *tbl, int level)
{
    for (int i = 0; i < 512; i++)
    {
        uint64_t e = tbl[i];
        if (!(e & X86_PTE_PRESENT))
            continue;
        if (!(e & X86_PTE_USER))
            continue;
        if (level > 1)
        {
            if ((level == 2) && (e & X86_PTE_HUGE))
                continue;
            uint64_t *next = (uint64_t *)(uintptr_t)(e & X86_PTE_ADDR_MASK);
            free_level(next, level - 1);
            pmm_free_frame(next);
        }
        else
        {
            if (e & X86_PTE_SHARED)
                continue; /* SHM-owned frame: the segment (not this root)
                             owns it, so the exit path must never free it.
                             There is no shm_destroy; segments are immortal,
                             and fork copies already dropped this bit. */
            pmm_free_frame((void *)(uintptr_t)(e & X86_PTE_ADDR_MASK));
        }
    }
}

void mmu_free_root(struct mmu_root *root)
{
    if (!root || root == kernel_root)
        return;
    free_level((uint64_t *)root->entry, 4);
    pmm_free_frame(root);
}

void mmu_switch(struct mmu_root *root)
{
    if (!root)
        root = kernel_root;
    write_cr3((uint64_t)(uintptr_t)root);
}

void *mmu_map_framebuffer(uintptr_t phys, size_t size)
{
    /* The GOP framebuffer lives behind PCIe: WB/WT caching turns every
       4-byte pixel store into a 64-byte read-modify-write over the bus.
       Map it WC (PAT=1, PCD=0, PWT=0 -> index 4) so stores accumulate in
       the CPU's 64-byte WC buffers and burst out in one PCIe transaction.
       Ensure the PAT slot exists even when this runs before vmm_init(). */
    mmu_pat_init();
    uintptr_t start = phys & ~(uintptr_t)0x1FFFFF;
    uintptr_t end = phys + size;
    uintptr_t first_virt = 0;

    /* Drop any stale cached lines from the previous WB/WT mapping before
       switching the memory type. */
    __asm__ volatile("wbinvd" ::: "memory");

    for (uintptr_t p = start; p < end; p += 0x200000)
    {
        unsigned int idx = (p >> 21) & 0x1FF;
        uint64_t entry = p | X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_HUGE;
        if (pat_available)
            entry |= X86_PTE_PAT_LARGE; /* WC: PAT=1, PCD=0, PWT=0 */
        else
            entry |= X86_PTE_PWT | X86_PTE_PCD; /* no PAT: safe UC fallback */
        pd_table2[idx] = entry;
        if (p == start)
            first_virt = 0x80000000ULL + idx * 0x200000ULL;
    }

    flush_tlb();
    return (void *)(first_virt + (phys & 0x1FFFFF));
}
