#ifndef AXIOME_HAL_BOOTINFO_H
#define AXIOME_HAL_BOOTINFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable description of the environment handed to the kernel by the
   bootloader. Populated by the architecture boot glue (e.g. axboot.c on
   x86_64). Portable kernel code reads this instead of reaching into
   bootloader structures directly, so a different boot protocol (or a
   different architecture) only has to fill this struct. */
struct hal_bootinfo
{
    int       framebuffer_present;
    uintptr_t fb_addr;
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;
    uint8_t   fb_bpp;
    uint8_t   fb_type;
    uint64_t  mmap_max_addr;
    void     *acpi_rsdp;
    char      cmdline[256];
};

struct hal_bootinfo *hal_bootinfo(void);

#ifdef __cplusplus
}
#endif

#endif
