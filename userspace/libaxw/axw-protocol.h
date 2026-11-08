#ifndef AXIOME_AXW_PROTOCOL_H
#define AXIOME_AXW_PROTOCOL_H

/* axiomeOS display server protocol (axw).
   Object-based wire protocol, inspired by Wayland concepts but our own
   design — no Wayland compatibility, no Linux UAPI.
   
   Wire message format (all fields little-endian, u32-aligned):
     offset 0:  u32 sender_object_id   (object the message is to/from)
     offset 4:  u16 opcode             (request or event number)
     offset 6:  u16 size               (total message size in bytes, incl. this header)
     offset 8:  u32 args[0..N]         (opcode-specific arguments)
   
   Object IDs:
     - bit 31 set (0x80000000): server-created global objects (registry, compositor, ...)
     - bit 31 clear: client-created objects (surfaces, buffers, ...)
   
   Transport: /devices/axw-0 character device (one fd per client connection).
   Pixels: shared memory via shm_create/shm_attach (GEM-backed), referenced by
   shmid in axw_shm_create_pool requests. */

#include <stdint.h>
#include <stddef.h>

/* ---- well-known global object IDs (server-created, bit 31 set) ---- */
#define AXW_ID_DISPLAY      0x80000001u
#define AXW_ID_REGISTRY     0x80000002u
#define AXW_ID_COMPOSITOR   0x80000003u
#define AXW_ID_SHM          0x80000004u
#define AXW_ID_SEAT         0x80000005u
#define AXW_ID_OUTPUT       0x80000006u

/* ---- display requests (client -> axw_display) ---- */
#define AXW_DISPLAY_SYNC           0
#define AXW_DISPLAY_GET_REGISTRY   1
#define AXW_DISPLAY_TERMINATE      2

/* ---- registry requests ---- */
#define AXW_REGISTRY_BIND          0

/* ---- compositor requests ---- */
#define AXW_COMPOSITOR_CREATE_SURFACE  0
#define AXW_COMPOSITOR_CREATE_REGION   1

/* ---- shm requests ---- */
#define AXW_SHM_CREATE_POOL            0

/* ---- buffer (client-created) ---- */

/* ---- surface requests (client-created) ---- */
#define AXW_SURFACE_DESTROY            0
#define AXW_SURFACE_ATTACH             1
#define AXW_SURFACE_DAMAGE             2
#define AXW_SURFACE_COMMIT             3
#define AXW_SURFACE_SET_TRANSFORM      4

/* ---- seat requests ---- */
#define AXW_SEAT_GET_POINTER           0
#define AXW_SEAT_GET_KEYBOARD          1

/* ---- output events (server -> client) ---- */
#define AXW_OUTPUT_GEOMETRY            0
#define AXW_OUTPUT_MODE                1
#define AXW_OUTPUT_DONE                2

/* ---- pointer events (server -> client) ---- */
#define AXW_POINTER_MOTION             0
#define AXW_POINTER_BUTTON             1
#define AXW_POINTER_AXIS               2
#define AXW_POINTER_FRAME              3

/* ---- keyboard events (server -> client) ---- */
#define AXW_KEYBOARD_KEYMAP            0
#define AXW_KEYBOARD_KEY               1
#define AXW_KEYBOARD_MODIFIERS         2

/* ---- callback events ---- */
#define AXW_CALLBACK_DONE              0

/* ---- pixel formats (mirrors drm_format) ---- */
#define AXW_FORMAT_XRGB8888 0
#define AXW_FORMAT_XBGR8888 1

/* ---- fixed-size wire message header ---- */
struct axw_msg_hdr {
    uint32_t sender_id;
    uint16_t opcode;
    uint16_t size;   /* total bytes, including this 8-byte header */
};

/* Maximum inline args per message (size = 8 + nargs*4). */
#define AXW_MAX_ARGS 16

/* ---- surface damage rectangle ---- */
struct axw_damage {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

/* ---- output geometry ---- */
struct axw_output_geometry {
    int32_t x;
    int32_t y;
    int32_t physical_width;
    int32_t physical_height;
    int32_t subpixel;
    int32_t make;
    int32_t model;
    int32_t transform;
};

/* ---- output mode ---- */
struct axw_output_mode {
    uint32_t flags;   /* 0x1 = current */
    int32_t width;
    int32_t height;
    int32_t refresh;  /* mHz */
};

/* ---- input device caps ---- */
#define AXW_POINTER_CAP_BUTTON  1u
#define AXW_KEYBOARD_CAP_KEY    1u

#endif
