#ifndef AXIOME_ARCH_MMU_H
#define AXIOME_ARCH_MMU_H

#include <stdint.h>
#include <stddef.h>

/* Architecture-independent MMU interface. vmm.c is portable policy on top of
   these primitives; each port implements them for its page-table format
   (x86_64: kernel/arch/x86_64/mmu.c, ARM64: TTBR/4-level, Xtensa: MMU). */

/* Portable page flags. Each port translates these to its real hardware bits. */
#define MMU_WRITE    (1u << 0)
#define MMU_USER     (1u << 1)
#define MMU_NX       (1u << 2)
#define MMU_HUGE     (1u << 3)
#define MMU_GLOBAL   (1u << 4)
#define MMU_UNCACHED (1u << 5)
#define MMU_WC       (1u << 6)
/* Shared frame: owned by whoever shares it (SHM segment), not by this
   mapping. The exit path never frees it; fork copies drop the mark (the
   copy is private). Must stay in sync with vmm.h. */
#define MMU_SHARED   (1u << 7)

/* Opaque handle to an address space. Only the arch MMU may dereference it. */
struct mmu_root;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the arch MMU: capture the active root, identity-map physical
   memory, etc. Called once from vmm_init(). */
void mmu_arch_init(void);

/* Active root of the current CPU context. */
struct mmu_root *mmu_current_root(void);

/* The kernel's shared address space. */
struct mmu_root *mmu_kernel_root(void);

/* A fresh user address space (kernel mappings shared, user mappings empty). */
struct mmu_root *mmu_new_user_root(void);

/* Deep-copy an address space (fork). */
struct mmu_root *mmu_clone_root(struct mmu_root *src);

/* Release every user page table owned by a user address space. */
void mmu_free_root(struct mmu_root *root);

/* Map/unmap one page. root == NULL means the kernel root. */
int mmu_map(struct mmu_root *root, uint64_t virt, uint64_t phys, uint32_t flags);
int mmu_unmap(struct mmu_root *root, uint64_t virt);

/* Translate a mapped virtual address (page offset preserved). */
uint64_t mmu_virt_to_phys(struct mmu_root *root, uint64_t virt);

/* True iff every page in [virt, virt+len) is mapped with the MMU_USER bit
   set in `root`, and (if write=1) the MMU_WRITE bit set. Used to validate
   userspace pointers before copy-in/copy-out. Does not allocate. */
int mmu_check_user_range(struct mmu_root *root, uint64_t virt, size_t len,
                         int write);

/* Update the page-table flags of an existing mapping (used to apply W^X
   protections after an image is loaded). Returns 0 or -1 if not mapped. */
int mmu_protect(struct mmu_root *root, uint64_t virt, uint32_t flags);

/* Activate an address space on the current CPU. */
void mmu_switch(struct mmu_root *root);

/* Map a physical region (typically the linear framebuffer) so it is usable
   from kernel context; returns a kernel virtual address. */
void *mmu_map_framebuffer(uintptr_t phys, size_t size);

#ifdef __cplusplus
}
#endif

#endif
