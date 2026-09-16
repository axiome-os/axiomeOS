#ifndef AXIOME_X86_64_MMU_H
#define AXIOME_X86_64_MMU_H

/* Internal x86_64 page-table definitions. Only arch-specific code (mmu.c,
   hal/x86_mmio.cpp) may use these; portable code uses the MMU_* flags from
   arch_mmu.h. */

#define X86_PTE_PRESENT (1UL << 0)
#define X86_PTE_WRITE   (1UL << 1)
#define X86_PTE_USER    (1UL << 2)
#define X86_PTE_PWT     (1UL << 3)
#define X86_PTE_PCD     (1UL << 4)
#define X86_PTE_ACCESSED (1UL << 5)
#define X86_PTE_DIRTY   (1UL << 6)
/* Bit 7 is context-sensitive: PS (huge) in PDP/PD entries, PAT in leaf PTEs.
   X86_PTE_HUGE and X86_PTE_PAT share the encoding; use the right alias for
   the level you are writing. */
#define X86_PTE_HUGE    (1UL << 7)
#define X86_PTE_PAT     (1UL << 7)
/* PAT bit for 2 MiB / 1 GiB large pages (bit 12). PTE huge + PAT_LARGE,
   with PCD=PWT=0, selects PAT index 4 (WC when PAT MSR is in the standard
   UEFI layout). */
#define X86_PTE_PAT_LARGE (1UL << 12)
#define X86_PTE_GLOBAL  (1UL << 8)
/* Bits 9-11 are CPU-available for OS use. Bit 9 marks SHM-shared frames:
   owned by the SHM segment (not by the mapping process), so free_level
   must never return them to the PMM while any mapping may reference them.
   fork's leaf copy masks it off (copies are private), exec/exit then free
   only the private copies. */
#define X86_PTE_SHARED  (1UL << 9)
#define X86_PTE_NX      (1UL << 63)

/* Physical-frame address bits of a page-table entry.  Masks off CPU flag
   bits (PCD/PWT/PAT/PAT_LARGE at bit 12, NX at bit 63, ...) that overlap or
   sit above the address field, so `entry & X86_PTE_ADDR_MASK` is always the
   canonical physical address of the referenced frame/table.  Upper- and
   leaf-level entries legitimately carry NX (bit 63) and PAT_LARGE (bit 12),
   so raw `& ~0xFFF` is *not* a valid way to recover an address. */
#define X86_PTE_ADDR_MASK (((1ULL << 52) - 1) & ~0xFFFULL & ~X86_PTE_NX)

#define IA32_PAT_MSR 0x277u
#define PAT_UC    0x00u
#define PAT_WC    0x01u
#define PAT_WT    0x04u
#define PAT_WP    0x05u
#define PAT_WB    0x06u
#define PAT_UC_MINUS 0x07u

/* Standard UEFI PAT slot for Write-Combining (PA4 = binary 100:
   PAT=1, PCD=0, PWT=0). */
#define PAT_WC_INDEX 4

/* Ensures IA32_PAT holds a WC (01h) entry; idempotent. Safe to call before
   vmm_init() (the early GOP mapping does so). */
void mmu_pat_init(void);
/* PAT slot selected for WC (valid after mmu_pat_init()). */
int mmu_pat_wc_index(void);
/* Non-zero when the CPU/firmware combination actually provides WC. When
   PAT itself is unsupported every WC request safely degrades to UC. */
int mmu_pat_has_wc(void);

#endif
