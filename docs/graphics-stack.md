# Graphics stack

## Layers (Phase 1-2 implemented)

```
 userspace (axw protocol client -> libaxw -> /devices/axw0)
                              |
 userspace (axdisplay-server: DRM master + axw protocol server)
                              |
 kernel/drm (DRM core + KMS + GEM, GOP backend)
   drm_device --------------- drm_ioctl dispatcher
     |                           |
   drm_connector          drm_gem_object (buffer objects, mmap)
   drm_crtc               drm_framebuffer (scanout wrapper)
   drm_plane
                              |
 kernel/framebuffer.c (text console, shares scanout fb)
                              |
 UEFI GOP (bootloader/gop.c captures mode, WC mapping)
```

## Device nodes

- `/dev/dri/card0` → `/devices/card0` — DRM device (ioctl + mmap + write-command compat)
- `/devices/axw0` — axw display server protocol (multiplexed, first opener = server)
- `/devices/dri0` — legacy DRI compat (write-based `axdri_cmd` protocol, kept for `gfx_test`)
- `/devices/input0` — unified GUI input queue

## DRM ioctls (write()-based `struct drm_cmd` + `SYS_IOCTL`)

```
GET_RESOURCES, GET_CONNECTOR, GET_CRTC, SET_CRTC, PAGE_FLIP, DIRTYFB,
GEM_CREATE, GEM_MMAP, GEM_FREE, GET_MODE, GET_CAPS, SET_MASTER, DROP_MASTER
```

## axw protocol objects

```
axw_display      — sync, get_registry, terminate
axw_registry     — bind globals
axw_compositor   — create_surface
axw_shm          — create_pool (GEM-backed shm segment)
axw_buffer       — client-created, from shm pool
axw_surface      — attach, damage, commit
axw_seat         — get_pointer, get_keyboard
axw_output       — geometry, mode, done
axw_callback     — done (frame callback)
```

Wire format: `u32 sender_id, u16 opcode, u16 size, u32 args[]` over `/devices/axw0`.

## What was removed/replaced

- `kernel/dri.c` write-command path is now the DRM core (`drm_sys_write` handles `struct drm_cmd`)
- `kernel/axdri_cmd.h` → `kernel/drm/drm_ioctl.h` (old header kept for `gfx_test` compat)
- `kernel/gfx/mesa_abi.h` → superseded by `drm_ioctl.h`
- `userspace/guixd.c`, `userspace/compositor.h`, `userspace/axclient.h`, `kernel/wm_abi.h` — legacy SHM seqlock protocol, still present but superseded by axw protocol

## What stays (by design)

- **GOP only** — no Intel/Bochs/virtio hardware drivers yet
- **Single compositor session** — no multi-seat/VT switching
- **`axgui_*` 2D primitives** — used by axdisplay-server for compositing
- **`axmui` HTML/CSS toolkit** — retained, render backend can migrate to axw buffers
