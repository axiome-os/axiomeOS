/* Host test for kernel/virtio_gpu.h struct layout and protocol values.
   Compiles the real header (never a copy) against the host toolchain. */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>

#include "virtio_gpu.h"

int main(void)
{
    /* PCI identifiers. */
    assert(VIRTIO_GPU_VENDOR == 0x1AF4);
    assert(VIRTIO_GPU_DEVICE == 0x1050);
    assert(VIRTIO_GPU_CLASS == 0x03);

    /* Virtio PCI config offsets. */
    assert(VIRTIO_PCI_STATUS == 0x06);
    assert(VIRTIO_PCI_COMMAND == 0x04);
    assert(VIRTIO_PCI_DEV_FEATURE == 0x34);
    assert(VIRTIO_PCI_DRV_FEATURE == 0x3C);
    assert(VIRTIO_PCI_QUEUE_SEL == 0x70);
    assert(VIRTIO_PCI_QUEUE_PFN == 0x78);

    /* Virtio status bits. */
    assert(VIRTIO_STATUS_ACK == (1U << 0));
    assert(VIRTIO_STATUS_DRIVER == (1U << 1));
    assert(VIRTIO_STATUS_DRIVER_OK == (1U << 2));
    assert(VIRTIO_STATUS_FEATURES_OK == (1U << 3));

    /* Virtio-gpu feature indices. */
    assert(VIRTIO_GPU_F_VIRGL == 0);
    assert(VIRTIO_GPU_F_EDID == 1);
    assert(VIRTIO_GPU_F_RESOURCE_BLOB == 3);
    assert(VIRTIO_GPU_F_CONTEXT_INIT == 4);

    /* Command types. */
    assert(VIRTIO_GPU_CMD_GET_DISPLAY_INFO == 0x0100);
    assert(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D == 0x0101);
    assert(VIRTIO_GPU_CMD_RESOURCE_UNREF == 0x0102);
    assert(VIRTIO_GPU_CMD_SET_SCANOUT == 0x0103);
    assert(VIRTIO_GPU_CMD_RESOURCE_FLUSH == 0x0105);
    assert(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D == 0x0107);
    assert(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING == 0x0108);
    assert(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING == 0x0109);
    assert(VIRTIO_GPU_CMD_CTX_CREATE == 0x0200);
    assert(VIRTIO_GPU_CMD_SUBMIT_3D == 0x0207);
    assert(VIRTIO_GPU_CMD_UPDATE_CURSOR == 0x0300);

    /* Response types. */
    assert(VIRTIO_GPU_RESP_OK_NODATA == 0x1100);
    assert(VIRTIO_GPU_RESP_OK_DISPLAY_INFO == 0x1101);
    assert(VIRTIO_GPU_RESP_OK_MAP_INFO == 0x1106);
    assert(VIRTIO_GPU_RESP_ERR_UNSPEC == 0x1200);
    assert(VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY == 0x1201);
    assert(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID == 0x1203);

    /* Display info structure layout. */
    assert(offsetof(struct virtio_gpu_resp_display_info, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resp_display_info, pmodes) == 24);
    assert(sizeof(struct virtio_gpu_resp_display_info) == 408);

    assert(offsetof(struct virtio_gpu_display_one, r) == 0);
    assert(offsetof(struct virtio_gpu_display_one, enabled) == 16);
    assert(offsetof(struct virtio_gpu_display_one, flags) == 20);
    assert(sizeof(struct virtio_gpu_display_one) == 24);

    assert(offsetof(struct virtio_gpu_rect, x) == 0);
    assert(offsetof(struct virtio_gpu_rect, y) == 4);
    assert(offsetof(struct virtio_gpu_rect, width) == 8);
    assert(offsetof(struct virtio_gpu_rect, height) == 12);
    assert(sizeof(struct virtio_gpu_rect) == 16);

    /* Ctrl header layout (24 bytes). */
    assert(offsetof(struct virtio_gpu_ctrl_hdr, type) == 0);
    assert(offsetof(struct virtio_gpu_ctrl_hdr, flags) == 4);
    assert(offsetof(struct virtio_gpu_ctrl_hdr, fence_id) == 8);
    assert(offsetof(struct virtio_gpu_ctrl_hdr, ctx_id) == 16);
    assert(offsetof(struct virtio_gpu_ctrl_hdr, ring_idx) == 20);
    assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24);

    /* 2D resource creation command layout. */
    assert(offsetof(struct virtio_gpu_resource_create_2d, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resource_create_2d, resource_id) == 24);
    assert(offsetof(struct virtio_gpu_resource_create_2d, format) == 28);
    assert(offsetof(struct virtio_gpu_resource_create_2d, width) == 32);
    assert(offsetof(struct virtio_gpu_resource_create_2d, height) == 36);
    assert(sizeof(struct virtio_gpu_resource_create_2d) == 40);

    /* Set scanout command layout. */
    assert(offsetof(struct virtio_gpu_set_scanout, hdr) == 0);
    assert(offsetof(struct virtio_gpu_set_scanout, r) == 24);
    assert(offsetof(struct virtio_gpu_set_scanout, scanout_id) == 40);
    assert(offsetof(struct virtio_gpu_set_scanout, resource_id) == 44);
    assert(sizeof(struct virtio_gpu_set_scanout) == 48);

    /* Resource unref layout. */
    assert(offsetof(struct virtio_gpu_resource_unref, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resource_unref, resource_id) == 24);
    assert(offsetof(struct virtio_gpu_resource_unref, padding) == 28);
    assert(sizeof(struct virtio_gpu_resource_unref) == 32);

    /* Attach backing command layout (header + nr_entries + mem entries). */
    assert(offsetof(struct virtio_gpu_resource_attach_backing, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resource_attach_backing, resource_id) == 24);
    assert(offsetof(struct virtio_gpu_resource_attach_backing, nr_entries) == 28);
    assert(sizeof(struct virtio_gpu_resource_attach_backing) == 32);
    assert(sizeof(struct virtio_gpu_resource_attach_backing) +
               sizeof(struct virtio_gpu_mem_entry) == 48);

    /* Detach backing layout. */
    assert(offsetof(struct virtio_gpu_resource_detach_backing, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resource_detach_backing, resource_id) == 24);
    assert(offsetof(struct virtio_gpu_resource_detach_backing, padding) == 28);
    assert(sizeof(struct virtio_gpu_resource_detach_backing) == 32);

    /* Mem entry layout (16 bytes). */
    assert(offsetof(struct virtio_gpu_mem_entry, addr) == 0);
    assert(offsetof(struct virtio_gpu_mem_entry, length) == 8);
    assert(offsetof(struct virtio_gpu_mem_entry, padding) == 12);
    assert(sizeof(struct virtio_gpu_mem_entry) == 16);

    /* Resource flush command layout. */
    assert(offsetof(struct virtio_gpu_resource_flush, hdr) == 0);
    assert(offsetof(struct virtio_gpu_resource_flush, r) == 24);
    assert(offsetof(struct virtio_gpu_resource_flush, resource_id) == 40);
    assert(sizeof(struct virtio_gpu_resource_flush) == 48);

    /* Transfer to host 2D layout. */
    assert(offsetof(struct virtio_gpu_transfer_to_host_2d, hdr) == 0);
    assert(offsetof(struct virtio_gpu_transfer_to_host_2d, r) == 24);
    assert(offsetof(struct virtio_gpu_transfer_to_host_2d, offset) == 40);
    assert(offsetof(struct virtio_gpu_transfer_to_host_2d, resource_id) == 48);
    assert(sizeof(struct virtio_gpu_transfer_to_host_2d) == 56);

    /* Vring descriptor layout (16 bytes). */
    assert(offsetof(struct vring_desc, addr) == 0);
    assert(offsetof(struct vring_desc, len) == 8);
    assert(offsetof(struct vring_desc, flags) == 12);
    assert(offsetof(struct vring_desc, next) == 14);
    assert(sizeof(struct vring_desc) == 16);

    /* Vring available ring (2 bytes header + entries). */
    assert(offsetof(struct vring_avail, flags) == 0);
    assert(offsetof(struct vring_avail, idx) == 2);
    assert(sizeof(struct vring_avail) == 4);

    /* Vring used ring (2 bytes header + entries). */
    assert(offsetof(struct vring_used, flags) == 0);
    assert(offsetof(struct vring_used, idx) == 2);
    assert(offsetof(struct vring_used_elem, id) == 0);
    assert(offsetof(struct vring_used_elem, len) == 4);
    assert(sizeof(struct vring_used_elem) == 8);

    /* Virtio-gpu format values match Linux header. */
    assert(VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM == 4);
    assert(VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM == 3);
    assert(VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM == 1);

    /* Device config struct packing. */
    assert(sizeof(struct virtio_gpu_config) == 20);

    /* Driver feature bits. */
    assert((1U << VIRTIO_GPU_F_RESOURCE_BLOB) == (1U << 3));
    assert((1U << VIRTIO_GPU_F_VIRGL) == 1U);

    /* Vring notify offset calculation. */
    assert((0x40 + 2 * 0) == 0x40);
    assert((0x40 + 2 * 1) == 0x42);

    printf("PASS virtio-gpu (struct/layout/protocol checks)\n");
    return 0;
}
