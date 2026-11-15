# AGENTS.md

Instructions for coding agents working in this repository.

axiomeOS is a freestanding x86_64 OS (UEFI bootloader, kernel, userspace). The
host compiler cannot build it. Use the Nix dev shell.

## Dev shell

Enter the shell from the repo root before building, running, or debugging:

```bash
nix develop
```

`flake.nix` is the supported environment. It provides:

- the `x86_64-elf` cross GCC, G++, binutils, and assembler (`pkgsCross.x86_64-embedded`)
- QEMU, GDB, GNU Make, Python 3, mtools, xorriso
- gnu-efi, exported as `EFI_PREFIX`
- an `objcopy` that can emit PE32+ EFI images, exported as `OBJCOPY`
- OVMF firmware, exported as `OVMF_FD`

Supported Nix systems: `x86_64-linux` and `x86_64-darwin` (Intel Macs). Darwin
uses `nixpkgs-26.05-darwin` because newer nixpkgs dropped `x86_64-darwin`.

`docs/toolchain.md` (hand-built `x86_64-elf` toolchain under `$HOME/opt/cross`)
is the fallback for a host without Nix. Do not mix that toolchain with the Nix
shell, and do not point `CC`, `CXX`, `AS`, or `LD` at the host compiler. The
top-level `Makefile` ignores environment `CC`/`CXX`/`AS`/`LD` on purpose: Nix's
stdenv exports the host tools, and those cannot link axiome ELFs.

Host-side unit tests are the exception. They use `HOSTCC` (`cc`) and `HOSTCXX`
(`c++`).

## Build and run

All of these assume `nix develop` is already active.

| Goal | Command |
|------|---------|
| Full image (`build/axiome.iso`, `build/disk.img`) | `make` |
| Kernel only | `make kernel` |
| Bootloader only | `make bootloader` |
| Userspace ELFs | `make userspace` |
| Bootable disk (incremental partitions) | `make disk.img` |
| Boot in QEMU | `make run` |
| 2D GPU fallback | `make run-2d` |
| Wait for GDB on `:1234` | `make debug` |
| Host unit tests | `make test` |
| Drop objects, keep `build/` | `make clean` |
| Delete `build/` | `make distclean` |

`make disk.img` rewrites only the partitions whose inputs changed and keeps
the rest of `build/disk.img` (including runtime state such as a first-boot
user). Use `make distclean` when that leftover state is getting in the way.

QEMU defaults differ by host, and the `Makefile` already selects them:

- Linux: SDL + VirGL (`virtio-vga-gl`), combined OVMF via `-bios`.
- macOS: Cocoa + plain `virtio-vga` (no virglrenderer), OVMF as a read-only
  pflash drive. `-bios` cannot load the split edk2 image QEMU ships on Darwin.

Serial output is the primary signal that the system booted. After a kernel,
boot, or userspace change, build the narrow target first, then `make run` when
the change is on the boot path. `build/` is generated; do not commit it.

GDB: `make debug`, then `gdb build/kernel/kernel.elf` and `target remote :1234`.

## Layout

Trust the tree over `docs/structure.md` when they disagree.

- `bootloader/` — UEFI `BOOTX64.EFI` (gnu-efi, axboot handoff)
- `kernel/` — ring 0. C and Intel-syntax `.S`, except the HAL
- `kernel/hal/`, `kernel/arch/x86_64/hal/` — the only C++ in the kernel
- `kernel/arch/x86_64/` — architecture code
- `userspace/` — user programs; libc lives in `userspace/libc`, not a top-level `libc/`
- `userspace/libaxw/` — window-system client library
- `include/` — shared headers (boot protocol, splash assets)
- `config/axboot.cfg` — `debug=` and `splash=`; copied into the boot partition
- `tools/` — image and manifest helpers
- `ports/mesa/` — upstream Mesa checkout. Read `ports/mesa/AGENTS.md` before touching it
- `ports/mesa-axiome/` — axiomeOS's Mesa port
- `docs/conventions.md` — coding style; follow it

## Language rules

- Kernel, libc, and userland are C11. Assembly is Intel syntax (`.intel_syntax noprefix`) in `.S` files.
- C++ is C++17, freestanding, and only inside the HAL. No exceptions, no RTTI, no libstdc++.
- Kernel memory: `kmalloc` / `kfree` and `pmm_alloc_frame` / `pmm_free_frame`. No host `malloc`. No allocation in interrupt handlers.
- Errors are `int`: `0` success, negative failure. Predicates return `bool`.
- Names: `snake_case` functions and variables, `SCREAMING_SNAKE` macros, `g_` globals, `s_` statics, `_t` typedefs. 4-space indent, K&R braces, 100-column lines. Details and examples are in `docs/conventions.md`.

## Third-party trees

`ports/mesa` is upstream Mesa, not axiomeOS code. Do not open merge requests or
comments against Mesa's GitLab. Some Mesa subtrees forbid AI changes; the list
is in `ports/mesa/AGENTS.md`. Port work belongs in `ports/mesa-axiome/` and
`docs/mesa-port.md`.

Clone with submodules: `git clone --recurse-submodules`.
