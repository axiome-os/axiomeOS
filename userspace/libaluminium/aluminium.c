/* aluminium.c — axiomeOS userspace client library for the Aluminium
 * graphics API.
 *
 * Wraps ioctls on /devices/aluminium0 and provides convenience helpers
 * for command buffer recording, submission, and presentation.
 *
 * Links against libc.sl (syscall wrappers, open/read/write/ioctl).
 */

#include "aluminium.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* IOCTL numbers — must match kernel/aluminium.h. */
#define AL_IOCTL_CREATE_DEVICE 0xA0
#define AL_IOCTL_GEM_CREATE    0xA2
#define AL_IOCTL_GEM_MMAP      0xA3
#define AL_IOCTL_GEM_FREE      0xA4
#define AL_IOCTL_SUBMIT        0xA5
#define AL_IOCTL_PRESENT       0xA6
#define AL_IOCTL_GET_MODE      0xA7
#define AL_IOCTL_GET_CAPS      0xA8

#define ALUMINIUM_PATH "/devices/aluminium0"

/* Userspace copies of kernel IOCTL structs (kernel/aluminium.h). */
struct aluminium_present {
    uint32_t gem_handle;
    uint32_t x, y, w, h;
};

struct aluminium_get_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
};

/* ---- global state ---- */

static int g_al_fd = -1;
static AlInstance g_al_instance = NULL;
static AlDevice g_al_device = NULL;
static AlQueue g_al_queue = NULL;

/* ---- public API ---- */

int al_axiome_open(void)
{
    if (g_al_fd >= 0)
        return 0;

    g_al_fd = open(ALUMINIUM_PATH, O_RDWR);
    if (g_al_fd < 0)
        return -1;

    AlInstanceCreateInfo ii = {
        .sType = AL_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    };
    AlResult res = alCreateInstance(&ii, &g_al_instance);
    if (res != AL_SUCCESS) {
        close(g_al_fd);
        g_al_fd = -1;
        return -1;
    }

    uint32_t count = 0;
    alEnumeratePhysicalDevices(g_al_instance, &count, NULL);
    if (count == 0) {
        alDestroyInstance(g_al_instance, NULL);
        g_al_instance = NULL;
        close(g_al_fd);
        g_al_fd = -1;
        return -1;
    }

    AlPhysicalDevice physdevs[8];
    count = 8;
    alEnumeratePhysicalDevices(g_al_instance, &count, physdevs);

    res = alCreateDevice(physdevs[0], NULL, &g_al_device);
    if (res != AL_SUCCESS) {
        alDestroyInstance(g_al_instance, NULL);
        g_al_instance = NULL;
        close(g_al_fd);
        g_al_fd = -1;
        return -1;
    }

    alGetDeviceQueue(g_al_device, 0, 0, &g_al_queue);
    return 0;
}

void al_axiome_close(void)
{
    if (g_al_device) {
        alDestroyDevice(g_al_device, NULL);
        g_al_device = NULL;
    }
    if (g_al_instance) {
        alDestroyInstance(g_al_instance, NULL);
        g_al_instance = NULL;
    }
    if (g_al_fd >= 0) {
        close(g_al_fd);
        g_al_fd = -1;
    }
    g_al_queue = NULL;
}

AlDevice al_axiome_device(void)
{
    return g_al_device;
}

AlQueue al_axiome_queue(void)
{
    return g_al_queue;
}

int al_axiome_buffer_alloc(uint64_t size, AlBuffer *out_buffer,
                           AlDeviceMemory *out_memory)
{
    if (!g_al_device || !out_buffer || !out_memory)
        return -1;

    AlBufferCreateInfo bi = {
        .sType = AL_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = AL_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 AL_BUFFER_USAGE_TRANSFER_DST_BIT |
                 AL_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    AlResult res = alCreateBuffer(g_al_device, &bi, out_buffer);
    if (res != AL_SUCCESS)
        return -1;

    AlMemoryAllocateInfo mi = {
        .sType = AL_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size,
        .memoryTypeIndex = 0,
    };
    res = alAllocateMemory(g_al_device, &mi, out_memory);
    if (res != AL_SUCCESS) {
        alDestroyBuffer(g_al_device, *out_buffer, NULL);
        return -1;
    }

    res = alBindBufferMemory(g_al_device, *out_buffer, *out_memory, 0);
    if (res != AL_SUCCESS) {
        alFreeMemory(g_al_device, *out_memory, NULL);
        alDestroyBuffer(g_al_device, *out_buffer, NULL);
        return -1;
    }

    return 0;
}

void *al_axiome_buffer_map(AlBuffer buffer, AlDeviceMemory memory,
                           uint64_t size)
{
    if (!g_al_device || !buffer || !memory)
        return NULL;

    void *ptr = NULL;
    AlResult res = alMapMemory(g_al_device, memory, 0, size, 0, &ptr);
    if (res != AL_SUCCESS)
        return NULL;
    return ptr;
}

void al_axiome_buffer_unmap(AlBuffer buffer, AlDeviceMemory memory)
{
    (void)buffer;
    if (!g_al_device || !memory)
        return;
    alUnmapMemory(g_al_device, memory);
}

void al_axiome_buffer_free(AlBuffer buffer, AlDeviceMemory memory)
{
    if (!g_al_device)
        return;
    if (memory) {
        alFreeMemory(g_al_device, memory, NULL);
    }
    if (buffer) {
        alDestroyBuffer(g_al_device, buffer, NULL);
    }
}

void al_cmd_fill_rect(AlCommandBuffer cb, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t rgb)
{
    (void)cb; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}

void al_cmd_clear(AlCommandBuffer cb, uint32_t rgb)
{
    (void)cb; (void)rgb;
}

void al_cmd_present(AlCommandBuffer cb, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h)
{
    (void)cb; (void)x; (void)y; (void)w; (void)h;
}

int al_axiome_submit(AlCommandBuffer cb, uint32_t cmd_count, AlFence fence)
{
    if (!g_al_device || !cb)
        return -1;

    (void)cmd_count;

    AlQueueSubmit submit = {
        .sType = AL_STRUCTURE_TYPE_QUEUE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };

    return alSubmitQueue(g_al_queue, 1, &submit, fence);
}

int al_axiome_wait_fence(AlFence fence, uint64_t timeout_ns)
{
    if (!g_al_device || !fence)
        return -1;
    AlResult res = alWaitForFences(g_al_device, 1, &fence, AL_TRUE, timeout_ns);
    return (res == AL_SUCCESS) ? 0 : -1;
}

int al_axiome_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (g_al_fd < 0)
        return -1;
    struct aluminium_present req;
    __builtin_memset(&req, 0, sizeof(req));
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    long r = ioctl(g_al_fd, AL_IOCTL_PRESENT, &req);
    return (r == 0) ? 0 : -1;
}

int al_axiome_get_mode(uint32_t *w, uint32_t *h, uint32_t *pitch,
                       uint32_t *bpp)
{
    if (g_al_fd < 0 || !w || !h || !pitch || !bpp)
        return -1;
    struct aluminium_get_mode mode;
    __builtin_memset(&mode, 0, sizeof(mode));
    long r = ioctl(g_al_fd, AL_IOCTL_GET_MODE, &mode);
    if (r != 0)
        return -1;
    *w = mode.width;
    *h = mode.height;
    *pitch = mode.pitch;
    *bpp = mode.bpp;
    return 0;
}
