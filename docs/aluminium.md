# Aluminium — axiomeOS Graphics API

Aluminium is axiomeOS's own hardware-agnostic, backend-agnostic graphics and
compute API. It is developed as a standalone git submodule at
`ports/aluminium/` (https://github.com/axiome-os/aluminium.git) and replaces
the previously-planned Mesa port.

## Design principles

- **Hardware agnostic** — the API never exposes GPU-specific concepts
  (vendor IDs, PCI BARs, register layouts). Resources, queues, and command
  buffers are opaque handles.
- **Backend agnostic** — the same application can run on top of Vulkan,
  OpenGL, a software rasterizer, or a custom kernel driver. Backends are
  pluggable modules discovered at runtime through `al_backend_register()`.
- **Portable** — designed with axiomeOS in mind, but the core library and ABI
  are not tied to axiomeOS. Builds as a standalone C library on any platform
  with a C11 compiler.
- **Explicit synchronization** — the library never inserts hidden barriers,
  fences, or flushes. The application owns all synchronization via
  `AlFence`, `AlSemaphore`, and `AlEvent`.
- **Extensible** — new features ship as extensions (`AL_EXT_*`), queried via
  `alEnumerateDeviceExtensionProperties` and loaded via
  `alGetDeviceProcAddr`. No "Aluminium 2.0" flag days.

## Contents

```
ports/aluminium/
├── include/aluminium/      # Public C API headers
├── src/                    # Core library: dispatch, sync, extensions
├── backends/
│   ├── software/           # CPU software rasterizer (works)
│   ├── axiome/               # axiomeOS DRM/GEM custom driver (skeleton)
│   ├── vulkan/               # Vulkan backend (stub)
│   └── opengl/               # OpenGL backend (stub)
├── tests/
│   └── smoke_test.c          # Passes against the software backend
├── CMakeLists.txt            # CMake build
└── meson.build               # Meson build
```

## Building

```sh
# From ports/aluminium/
meson setup build -Dbackend_software=true -Denable_tests=true
ninja -C build
meson test -C build
```

or with CMake:

```sh
cmake -S ports/aluminium -B build \
  -DALUMINIUM_BACKEND_SOFTWARE=ON \
  -DALUMINIUM_ENABLE_TESTS=ON
cmake --build build
ctest --test-dir build
```

## axiomeOS integration

Aluminium is consumed as a git submodule. The axiomeOS `axiome` backend maps
command-buffer operations onto the kernel DRM/GEM/ioctl surface. No new
kernel memory manager is required — GEM buffer objects plus the existing
`vmm_map_page(MMU_USER|WRITE)` path provide userspace access.

Userspace GUI apps link `libaluminium` alongside `libc.sl` via the axiomeOS
dynamic linker (`kernel/dynlink.c`).

The `software` backend is fully functional and serves as the reference
implementation and fallback when no hardware backend is available.
