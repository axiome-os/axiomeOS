# axiomeOS — GUI Stack Refactor Plan

**Goal:** Make the kernel manage graphics objects (DRM/KMS/GEM-style) instead of being a dumb blit pipe, and replace the ad-hoc SHM seqlock compositor/client ABI with a proper object-based display server protocol. Minimal refactor: no Wayland compat, GOP only, single session, `/dev/dri/card0`.

## Status

**Phase 1 (DRM/KMS/GEM) — DONE:** Kernel builds and boots with DRM core, KMS objects, GEM buffer objects, GOP backend. `/dev/dri/card0` registered. `SYS_IOCTL` added.

**Phase 2 (axw protocol + compositor) — DONE:** `libaxw` client library, `axw-protocol.h` wire format, `/devices/axw0` multiplexing device, `axdisplay-server` compositor, `axinfo` ported to axw protocol. init starts axdisplay-server on boot.

**Phase 3+ (remaining apps, input evdev) — TODO:** See below.

---

## Design constraints (accepted)

1. **Not Wayland-compatible** — our own object-based protocol, inspired by Linux DRM/KMS concepts.
2. **GOP only** — no new hardware drivers (Intel/Bochs/virtio stay stubs or are removed).
3. **Single compositor session** — no logind/multi-seat/VT switching.
4. **Minimal refactor** — reuse `axgui_*` primitives, `axmui`, `framebuffer.c`, existing syscalls where possible.
5. **`/dev/dri/card0`** — device node path (compat alias for `/devices/dri0` acceptable).

---

## Current state ("dumb" kernel)

```
UEFI GOP → framebuffer.c (WC back buffer, dirty rect, CPU memcpy)
                    → dri.c (dumb buffers, write()-ioctl, CPU blit present)
                    → input.c (ring queue, exclusive grab)
guixd/axwm own scanout, blit client SHM under title bars each frame
axgui/axmui draw CPU-only into SHM; compositor PRESENTs whole screen
Protocol: wm_abi.h SHM seqlock + axclient.h glue (ad-hoc)
```

---

## Target state

```
Kernel: DRM core + KMS + GEM wrapping GOP
        → /dev/dri/card0 (DRM device)
        → drm_connector/crtc/plane/framebuffer objects
        → drm_gem_object buffer objects with mmap
        → input core + evdev (/dev/input/event*)
Userspace: axw protocol over IPC/socket
        → libaxw client library (marshalling, proxies)
        → axdisplay-server (compositor, owns DRM master)
        → axgui_* render into protocol buffers (GEM-backed)
        → page-flip present (not full CPU blit every frame)
```

---

## Phase 1: Kernel DRM/KMS/GEM core (wrap GOP)

**New files:**
- `kernel/drm/drm.h` / `kernel/drm/drm.c` — DRM core: `drm_device`, `drm_file`, `drm_minor`, ioctl dispatcher, `/dev/dri/card0` registration.
- `kernel/drm/kms.h` / `kernel/drm/kms.c` — KMS objects: connector, encoder, crtc, plane, framebuffer, mode struct, mode config init/cleanup, `MODE_GETRESOURCES`, `MODE_GETCONNECTOR`, `MODE_GETCRTC`, `MODE_SETCRTC`, `MODE_PAGE_FLIP`, `MODE_DIRTYFB`.
- `kernel/drm/gem.h` / `kernel/drm/gem.c` — GEM buffer objects: `drm_gem_object` (refcounted, pages, mmap), `GEM_CREATE`, `GEM_MMAP`, `GEM_FREE`.
- `kernel/drm/gop_drm.c` — GOP as KMS driver: single connector/crtc/plane, adopts UEFI GOP mode, CPU present via `fb_flush`, no acceleration.
- `kernel/drm/drm_ioctl.h` — ioctl numbers and command structs (freestanding, shared with userspace tests).

**Modified:**
- `kernel/dri.c` — reduced to thin compat shim on top of DRM/GEM (or removed).
- `kernel/framebuffer.c` — text console renders into DRM framebuffer/GEM; simplify back-buffer logic.
- `kernel/gfx/manager.cpp` — DRM driver registration replaces `IDisplay` backend registry (or `IDisplay` wraps DRM KMS).
- `kernel/gfx/gop_display.cpp` — GOP becomes KMS backend; caps stay 0 (simplified).
- `kernel/syscall.c` — replace `/devices/dri` string gate with DRM master auth (`SET_MASTER`/`DROP_MASTER`).
- `kernel/syscall_numbers.h` — add `SYS_IOCTL` if needed.

**Deleted/deprecated:**
- `kernel/axdri_cmd.h` — replaced by `drm_ioctl.h`.
- `kernel/gfx/mesa_abi.h` — replaced by DRM ioctl headers.

**IOCTLs (minimal):**
```
GET_RESOURCES, GET_CONNECTOR, GET_CRTC, SET_CRTC, PAGE_FLIP, DIRTYFB,
GEM_CREATE, GEM_MMAP, GEM_FREE, GET_MODE, GET_CAPS, SET_MASTER, DROP_MASTER
```

---

## Phase 2: Custom display server protocol (axw)

**New files:**
- `userspace/libaxw/axw-protocol.h` — wire format: object id (u32), opcode (u16), size (u16), args (u32[]). Requests + events.
- `userspace/libaxw/axw-client.h/.c` — client library: marshalling, display connect, proxies, event queue.
- `userspace/axdisplay-server.c` — compositor server (replaces `guixd.c`): owns DRM master, event loop, surface tree, damage tracking, present/page-flip.

**Protocol objects (minimal):**
```
axw_display      — sync, get_registry, terminate
axw_registry     — bind globals
axw_compositor   — create_surface, create_region
axw_shm          — create_pool (GEM handle), format list
axw_buffer       — from shm pool, attach to surface
axw_surface      — attach, damage, commit
axw_seat         — get_pointer, get_keyboard
axw_pointer      — motion, button, axis, frame
axw_keyboard     — keymap, key, modifiers
axw_output       — geometry, mode, done, scale
axw_callback     — done (frame callback)
```

Transport: axiome IPC (`SYS_IPC_*`) or unix socket; `/devices/axw-0` or `/run/axw-0`.

**Modified:**
- `kernel/wm_abi.h` — deprecated, replaced by `axw-protocol.h`.
- `userspace/axgui.h` — `axgui_fb` wraps protocol buffer (GEM handle + mmap pointer); `axgui_present` → `axw_surface_attach` + `commit`.
- `userspace/axclient.h` — removed, replaced by `libaxw`.
- `userspace/compositor.h` — removed, replaced by `axdisplay-server.c`.

---

## Phase 3: Compositor + clients (single session)

- Compositor opens `/dev/dri/card0`, becomes DRM master, queries connector/crtc mode.
- Clients connect via protocol, create shm pools (GEM buffers), render with `axgui_*` into buffer, attach to `axw_surface`, commit.
- Compositor composites surfaces onto scanout fb, then `PAGE_FLIP` (or `fb_flush` on GOP).
- Input routed to focused surface via protocol.
- Single session, one seat.

**Modified:**
- `userspace/guixd.c` — removed.
- `userspace/axwm.c` — removed.
- `userspace/axterm.c`, `axinfo.c`, `axlogin.c`, `axoobe.c` — ported to `libaxw`.

---

## Phase 4: Input (evdev-like, minimal)

**New files:**
- `kernel/input/input.c` — input core (`input_dev`, `input_register_device`).
- `kernel/input/evdev.c` — evdev protocol (`struct input_event`, `EVIOCG*`, `EVIOCGRAB`), `/dev/input/event*`.

**Modified:**
- `kernel/input.c` — replaced by input core + evdev.
- `kernel/keyboard.c`, `kernel/mouse.c`, `kernel/hid_boot.c` — register as input devices.
- Remove `g_grabbed` exclusive-grab model.

---

## Phase 5: Tests

- `tests/drm-ioctl-test.c` — host test for ioctl encoding.
- `tests/gem-mmap-test.c` — host test for GEM mmap offset/handle math.
- `tests/axw-protocol-test.c` — host test for wire format marshalling.
- QEMU boot: DRM modeset on GOP; compositor starts; client draws via protocol; page-flip shows pixels.

---

## File summary

| Phase | New | Modified | Removed |
|-------|-----|----------|---------|
| 1. DRM/KMS/GEM | ~8 (kernel/drm/*) | dri.c, framebuffer.c, gfx/manager.cpp, gfx/gop_display.cpp, syscall.c, syscall_numbers.h | axdri_cmd.h, mesa_abi.h |
| 2. Protocol | ~4 (libaxw/*) + axdisplay-server.c | wm_abi.h, axgui.h | axclient.h, compositor.h |
| 3. Compositor | — | axterm/axinfo/axlogin/axoobe, axgui.h | guixd.c, axwm.c |
| 4. Input | input.c, evdev.c | input.c, keyboard.c, mouse.c, hid_boot.c | — |
| 5. Tests | 3 test files | — | — |

---

## What stays unchanged

- **GOP** is the only display backend.
- **Single compositor session.**
- **`axgui_*` 2D primitives** — kept, render into protocol buffers.
- **`axmui` HTML/CSS toolkit** — kept.
- **No Wayland compatibility.**
