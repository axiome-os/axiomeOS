#ifndef AXIOME_LIB_ALUMINIUM_H
#define AXIOME_LIB_ALUMINIUM_H

/* Public Aluminium C API for axiomeOS userspace.
 *
 * This header re-exports the cross-platform Aluminium header from
 * ports/aluminium/include/aluminium/aluminium.h and adds axiomeOS-specific
 * convenience wrappers on top.
 *
 * Applications link against libaluminium (built as libc.sl companion or
 * standalone shared object) and call these functions to submit command
 * buffers, allocate GEM buffers, and present to scanout.
 */

#include <aluminium/aluminium.h>
#include <aluminium/aluminium_platform.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- *
 * AxiomeOS-specific convenience layer                              *
 * ---------------------------------------------------------------- */

/* Open a connection to /devices/aluminium0 and create an Aluminium
 * instance + device in one call. Returns 0 on success, -1 on failure. */
int al_axiome_open(void);

/* Close the connection and destroy the device/instance. */
void al_axiome_close(void);

/* Get the currently open device (NULL if not opened). */
AlDevice al_axiome_device(void);

/* Get the default graphics queue. */
AlQueue al_axiome_queue(void);

/* Allocate a GEM buffer of `size` bytes and bind it to a fresh memory
 * allocation. Returns AL_SUCCESS or a negative error code. */
int al_axiome_buffer_alloc(uint64_t size, AlBuffer *out_buffer,
                           AlDeviceMemory *out_memory);

/* Map a GEM buffer CPU-visible. Returns the mapped pointer or NULL. */
void *al_axiome_buffer_map(AlBuffer buffer, AlDeviceMemory memory,
                           uint64_t size);

/* Unmap a previously mapped buffer. */
void al_axiome_buffer_unmap(AlBuffer buffer, AlDeviceMemory memory);

/* Free a buffer and its memory. */
void al_axiome_buffer_free(AlBuffer buffer, AlDeviceMemory memory);

/* Record a fill-rect command into a command buffer. */
void al_cmd_fill_rect(AlCommandBuffer cb, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t rgb);

/* Record a clear command. */
void al_cmd_clear(AlCommandBuffer cb, uint32_t rgb);

/* Record a present command (dirty rect). */
void al_cmd_present(AlCommandBuffer cb, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h);

/* Submit a command buffer to the default queue, signaling `fence`. */
int al_axiome_submit(AlCommandBuffer cb, uint32_t cmd_count, AlFence fence);

/* Wait for a fence (with timeout in nanoseconds, 0 = no timeout). */
int al_axiome_wait_fence(AlFence fence, uint64_t timeout_ns);

/* Present a dirty rect directly to scanout. */
int al_axiome_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Get the current display mode. */
int al_axiome_get_mode(uint32_t *w, uint32_t *h, uint32_t *pitch,
                       uint32_t *bpp);

#ifdef __cplusplus
}
#endif

#endif /* AXIOME_LIB_ALUMINIUM_H */
