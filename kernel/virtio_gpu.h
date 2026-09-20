#ifndef AXIOME_VIRTIO_GPU_H
#define AXIOME_VIRTIO_GPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIRTIO_GPU_VENDOR      0x1AF4
#define VIRTIO_GPU_DEVICE      0x1050
#define VIRTIO_GPU_CLASS       0x03
#define VIRTIO_GPU_SUBCLASS    0x00
#define VIRTIO_GPU_PROG_IF     0x00

#define VIRTIO_PCI_STATUS          0x06
#define VIRTIO_PCI_COMMAND         0x04
#define VIRTIO_PCI_DEV_FEATURE_SEL 0x30
#define VIRTIO_PCI_DEV_FEATURE     0x34
#define VIRTIO_PCI_DRV_FEATURE_SEL 0x38
#define VIRTIO_PCI_DRV_FEATURE     0x3C
#define VIRTIO_PCI_QUEUE_SEL       0x70
#define VIRTIO_PCI_QUEUE_READY     0x72
#define VIRTIO_PCI_QUEUE_NOTIFY    0x74
#define VIRTIO_PCI_QUEUE_STATUS    0x76
#define VIRTIO_PCI_QUEUE_PFN       0x78

#define VIRTIO_STATUS_ACK           (1U << 0)
#define VIRTIO_STATUS_DRIVER        (1U << 1)
#define VIRTIO_STATUS_DRIVER_OK     (1U << 2)
#define VIRTIO_STATUS_FEATURES_OK   (1U << 3)

#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1
#define VIRTIO_GPU_F_RESOURCE_UUID  2
#define VIRTIO_GPU_F_RESOURCE_BLOB  3
#define VIRTIO_GPU_F_CONTEXT_INIT   4
#define VIRTIO_GPU_F_BLOB_ALIGNMENT 5

struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
    uint32_t blob_alignment;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed));

#define VIRTIO_GPU_UNDEFINED            0x0000
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF   0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT      0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH   0x0105
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0107
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0108
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0109

#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103
#define VIRTIO_GPU_RESP_OK_EDID         0x1104
#define VIRTIO_GPU_RESP_OK_RESOURCE_UUID 0x1105
#define VIRTIO_GPU_RESP_OK_MAP_INFO     0x1106
#define VIRTIO_GPU_RESP_ERR_UNSPEC      0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY 0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134,
};

#define VIRTIO_GPU_CMD_CTX_CREATE      0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY     0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D     0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB  0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB 0x0209

#define VIRTIO_GPU_CMD_UPDATE_CURSOR 0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR   0x0301

#define VIRTIO_DESC_F_NEXT   1
#define VIRTIO_DESC_F_WRITE  2

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[0];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[0];
} __attribute__((packed));

#define VIRTIO_GPU_MAX_VRING_ENTRIES 256

struct virtio_gpu_vring {
    int idx;
    uint32_t entries;
    uint64_t pfn;
    uint32_t num_pages;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    uint16_t avail_idx;
    uint16_t used_idx;
    volatile uint8_t *mmio;
};

struct virtio_gpu_device {
    int detected;
    int initialized;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint64_t mmio_phys;
    uint32_t mmio_size;
    uint32_t irq;
    volatile uint8_t *mmio;
    uint32_t device_features;
    uint32_t driver_features;
    struct virtio_gpu_vring control;
    struct virtio_gpu_vring scanout;
    int has_display_info;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t scanout_pitch;
    int scanout_bpp;
    uint32_t fb_resource_id;
    uint64_t fb_phys;
    void *fb_virt;
    uint32_t fb_size;
    /* Per-device command/response buffers (identity-mapped,
       used as both CPU-accessible and device-readable). */
    uint64_t cmd_phys;
    void *cmd_virt;
    uint64_t resp_phys;
    void *resp_virt;
    /* Modern transport: offsets within BAR for common/device config */
    uint32_t common_offset;
    uint8_t common_bar;
    uint32_t device_offset;
    uint8_t device_bar;
    uint16_t notify_offset_multiplier;
    volatile uint8_t *common_cfg; /* BAR + common_offset */
};

int virtio_gpu_probe(void);
int virtio_gpu_init(struct virtio_gpu_device *vgpu);
void virtio_gpu_cleanup(struct virtio_gpu_device *vgpu);
int virtio_gpu_submit(struct virtio_gpu_device *vgpu,
                        void *cmd, size_t cmd_len,
                        void *resp, size_t resp_len,
                        int timeout_ms);
struct virtio_gpu_device *virtio_gpu_dev(void);

#ifdef __cplusplus
}
#endif

#endif
