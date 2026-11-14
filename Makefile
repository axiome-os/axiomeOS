REPO_ROOT := $(realpath .)
BUILD_DIR := $(REPO_ROOT)/build

# Tool locations are overridable via environment or command line, e.g.:
#   make CROSS_PREFIX=/opt/axiome/cross/bin
#   make CC=/path/to/x86_64-elf-gcc LD=/path/to/x86_64-elf-ld   (command line only)
#   make OVMF=/path/to/OVMF.fd EFI_PREFIX=/usr SUDO=
PYTHON       ?= python3
# Host toolchain for the native unit tests. `cc`/`c++` resolve to the system
# compiler everywhere (gcc on Linux, clang on macOS, the nix stdenv wrappers
# inside `nix develop`); bare gcc is not present in every environment.
HOSTCC       ?= cc
HOSTCXX      ?= c++
CROSS_TRIPLE ?= x86_64-elf
CROSS_PREFIX ?= $(HOME)/opt/cross/bin
# OVMF_FD is exported by the nix dev shell (flake.nix).
OVMF         ?= $(if $(OVMF_FD),$(OVMF_FD),/usr/share/ovmf/OVMF.fd)
SUDO         ?= sudo

# The edk2 code image shipped with qemu on macOS is a split firmware that
# `-bios` refuses to load ("could not load PC BIOS"); it has to be exposed as
# a (read-only) pflash device. Linux's combined OVMF.fd works with -bios.
ifeq ($(shell uname -s),Darwin)
OVMF_ARGS    := -drive if=pflash,format=raw,readonly=on,file=$(OVMF)
else
OVMF_ARGS    := -bios $(OVMF)
endif

# Use the cross toolchain from PATH if there is one (nix dev shell),
# otherwise fall back to CROSS_PREFIX.
ifeq ($(shell command -v $(CROSS_TRIPLE)-gcc 2>/dev/null),)
CROSS_BIN := $(CROSS_PREFIX)/
endif

# Cross-compiler for building kernel + userspace (freestanding axiome ELFs).
#
# Do NOT use ?= for these: make predefines CC=cc, CXX=g++, AS=as, LD=ld and
# nix's stdenv exports CC/CXX/AS/LD too, so ?= silently kept the host tools
# (on macOS: Apple's ld, which has no -shared). Only a command-line override
# (make CC=...) replaces the cross tools.
ifeq ($(filter command line override,$(origin CC)),)
CC  := $(CROSS_BIN)$(CROSS_TRIPLE)-gcc
endif
ifeq ($(filter command line override,$(origin CXX)),)
CXX := $(CROSS_BIN)$(CROSS_TRIPLE)-g++
endif
ifeq ($(filter command line override,$(origin AS)),)
AS  := $(CROSS_BIN)$(CROSS_TRIPLE)-as
endif
ifeq ($(filter command line override,$(origin LD)),)
LD  := $(CROSS_BIN)$(CROSS_TRIPLE)-ld
endif
# Hand the cross tools to the kernel/ and bootloader/ sub-makes.
export CC CXX AS LD

DEV ?= /dev/sdx

# QEMU 3D: virtio-gpu VirGL. Requires host virglrenderer + GL display (Linux-like).
# See ~/linux/drivers/gpu/drm/virtio/virtgpu_drv.c:features[] + virtio_has_feature().
# Default is 3D (virgl) like Linux DRM_VIRTIO_GPU_KMS; for 2D fallback:
#   make run-2d                # sdl + virtio-vga (no virgl, no gl)
#   make run QEMU_DISPLAY=sdl QEMU_GPU_DEVICE=virtio-vga
# For Venus/GFXStream (blob): make run QEMU_GPU_DEVICE="virtio-gpu-gl-pci,hostmem=4G,blob=true"
# macOS has no virglrenderer, so it defaults to Cocoa + plain virtio-vga.
ifeq ($(shell uname -s),Darwin)
QEMU_DISPLAY    ?= cocoa
QEMU_GPU_DEVICE ?= virtio-vga
QEMU_DISPLAY_2D ?= cocoa
else
QEMU_DISPLAY    ?= sdl,gl=on
QEMU_GPU_DEVICE ?= virtio-vga-gl
QEMU_DISPLAY_2D ?= sdl
endif
# Linux-like aliases: discrete vs integrated VGA variant
QEMU_GPU_2D ?= virtio-vga

# Partition geometry (must stay in sync with tools/mkpart.py).
BOOT_PART_LBA     := 2048
BOOT_PART_SECTORS := 129024
ROOT_PART_LBA     := 131072
ROOT_PART_SECTORS := 393216
DISK_SECTORS      := 524288          # 256 MiB

# Each partition is built as its own artifact, so `make disk.img` only re-runs
# the steps whose inputs actually changed instead of reformatting the whole
# disk from scratch on every edit. disk.img itself is assembled in place with
# `dd conv=notrunc`, so untouched partitions (and any runtime state on them,
# e.g. a user created by the first-boot OOBE) are preserved.
BOOT_FAT  := $(BUILD_DIR)/boot.fat
ROOT_AXFS := $(BUILD_DIR)/root.axfs
DISK_PATH := $(BUILD_DIR)/disk.img

# Host files embedded verbatim via `bin:` lines in root_manifest.txt. When any
# of them changes (a userspace .elf or a .kxt), the ROOT partition rebuilds.
MANIFEST_BINS := $(shell sed -n 's/.*[[:space:]]bin:\([^[:space:]]*\).*/\1/p' root_manifest.txt)

.PHONY: all kernel bootloader userspace iso run run-2d run-3d run-venus run-iso run-fb run-usb debug test test-hid clean distclean install disk.img assets

all: iso

KERNEL_ELF     := $(BUILD_DIR)/kernel/kernel.elf
BOOTLOADER_EFI := $(BUILD_DIR)/bootloader/BOOTX64.EFI

kernel: $(KERNEL_ELF)

bootloader: $(BOOTLOADER_EFI)

# The sub-makes own dependency tracking; the top level only needs to run them
# before anything compares their outputs (the ESP image, the disk image).
$(KERNEL_ELF): userspace include/bootsplash_assets.h
	$(MAKE) -C kernel BUILD_DIR=$(BUILD_DIR)/kernel

# Userspace build variables (defined before the target so prerequisites expand at parse time)
USER_PROGS := init hello cat echo sh ls mkdir cp mv rm touch whoami uname \
	login su chmod chown vfs_test ipc_test proc_test driver_test net_test \
	kill kxtload kxtunload syslogd oobe date burn forkt ping \
	axterm evtest axinfo axlogin axoobe axdisplay-server
USER_ELFS := $(addprefix userspace/,$(addsuffix .elf,$(USER_PROGS)))

USER_BASE_CFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	-fno-stack-protector -fPIC -mcmodel=small -O2 -g -pipe \
	-mno-red-zone -Wall -Wextra -I userspace -I userspace/libc -I userspace/libaxw \
	-I$(REPO_ROOT)/kernel
USER_CFLAGS := $(USER_BASE_CFLAGS) -fpie

LIBC_CFLAGS := $(USER_BASE_CFLAGS)

CRT0_OBJ := $(BUILD_DIR)/user_crt0.o

LIBC_SRCS := userspace/libc/errno.c userspace/libc/string.c \
	userspace/libc/stdlib.c userspace/libc/stdio.c \
	userspace/libc/syscall.c userspace/libc/signal.c \
	userspace/libc/time.c
LIBC_OBJS := $(patsubst userspace/libc/%.c, $(BUILD_DIR)/libc_%.o, $(LIBC_SRCS))
LIBC_SL := userspace/libc.sl

LIB_AXW_OBJ := $(BUILD_DIR)/libaxw_axw-client.o

userspace: $(USER_ELFS) $(LIBC_SL)
	@:

$(BUILD_DIR)/user_crt0.o: userspace/libc/crt0.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_%.o: userspace/libc/%.c
	@mkdir -p $(@D)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

$(LIB_AXW_OBJ): userspace/libaxw/axw-client.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -I userspace/libaxw -c $< -o $@

$(LIBC_SL): $(LIBC_OBJS) userspace/libc.ld
	@mkdir -p $(@D)
	$(LD) -shared -T userspace/libc.ld --hash-style=sysv \
		-soname libc.sl -z max-page-size=0x1000 -o $@ $(LIBC_OBJS)

userspace/%.elf: $(BUILD_DIR)/%_user.o $(CRT0_OBJ) $(LIBC_SL) $(LIB_AXW_OBJ)
	@mkdir -p $(@D)
	$(LD) -pie -T userspace/link.ld --hash-style=sysv \
		-z max-page-size=0x1000 -e _start -o $@ $^

$(BUILD_DIR)/%_user.o: userspace/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Host-side axiomefs formatter (enhanced to take a disk image + offset).
$(BUILD_DIR)/kernel/mkfs_axiomefs: tools/mkfs_axiomefs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -I $(REPO_ROOT) -o $@ $<

# The UEFI loader binary. Rebuilt by the bootloader sub-make when any of its
# sources or the shared splash headers change.
$(BOOTLOADER_EFI): $(wildcard bootloader/*.c bootloader/*.h bootloader/Makefile include/axboot.h) \
		include/bootsplash.h include/bootsplash_assets.h
	$(MAKE) -C bootloader

iso: $(BOOT_FAT)
	rm -rf $(BUILD_DIR)/isowork
	mkdir -p $(BUILD_DIR)/isowork
	cp $(BOOT_FAT) $(BUILD_DIR)/isowork/esp.img
	xorriso -as mkisofs -R -f -e esp.img -no-emul-boot \
		-o $(BUILD_DIR)/axiome.iso $(BUILD_DIR)/isowork 2>/dev/null

# BOOT partition: a FAT32 ESP image holding the UEFI bootloader and the
# kernel. Rebuilt when either of them changes. The bootloader reads
# \kernel.elf from the volume root via SimpleFileSystem.
$(BOOT_FAT): $(BUILD_DIR)/kernel/kernel.elf $(BUILD_DIR)/bootloader/BOOTX64.EFI config/axboot.cfg
	@mkdir -p $(dir $@)
	rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=$(BOOT_PART_SECTORS) 2>/dev/null
	mformat -F -i $@ -v BOOT ::
	mcopy -i $@ $(BUILD_DIR)/kernel/kernel.elf ::kernel.elf
	mcopy -i $@ config/axboot.cfg ::axboot.cfg
	mmd -i $@ ::EFI ::EFI/BOOT
	mcopy -i $@ $(BUILD_DIR)/bootloader/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI

# ROOT partition: a standalone axiomefs volume populated from the manifest.
# mkfs_axiomefs gets offset 0 because this file *is* the whole partition.
$(ROOT_AXFS): $(BUILD_DIR)/kernel/mkfs_axiomefs root_manifest.txt $(MANIFEST_BINS)
	@mkdir -p $(dir $@)
	rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_PART_SECTORS) 2>/dev/null
	$(BUILD_DIR)/kernel/mkfs_axiomefs $@ 0 root_manifest.txt

# Assemble a 256MB MBR disk:
#   * partition 0: FAT32 ESP "BOOT" (LBA 2048) holding EFI/BOOT/BOOTX64.EFI
#     and kernel.elf at the volume root
#   * partition 1: axiomefs "ROOT" (LBA 131072) populated from root_manifest.txt
# The disk image is only zeroed/partitioned when missing or when mkpart.py
# changed; existing partitions are overlaid into it only when they changed.
#
# Staleness is tracked with per-partition stamp files (build/.stamp-boot,
# build/.stamp-root), NOT with disk.img's mtime: every QEMU run bumps the
# image mtime through guest writes (journal, OOBE), which used to make a
# just-built partition look older than the image and silently skip its
# overlay forever (stale kernel with new userspace). QEMU never touches the
# stamps, so a missed overlay self-heals on the next build.
BOOT_STAMP := $(BUILD_DIR)/.stamp-boot
ROOT_STAMP := $(BUILD_DIR)/.stamp-root
$(DISK_PATH): $(BUILD_DIR)/kernel/mkfs_axiomefs $(BOOT_FAT) $(ROOT_AXFS) tools/mkpart.py
	@mkdir -p $(BUILD_DIR)
	@target=$(DISK_PATH); \
		if [ ! -f "$$target" ] || find tools/mkpart.py -newer "$$target" >/dev/null 2>&1; then \
		printf '%s\n' 'disk.img: creating fresh image + MBR'; \
		dd if=/dev/zero of="$$target" bs=512 count=$(DISK_SECTORS) 2>/dev/null; \
		$(PYTHON) tools/mkpart.py "$$target"; \
		need_boot=1; need_root=1; \
	else \
		need_boot=0; need_root=0; \
	fi; \
	if [ ! -f "$(BOOT_STAMP)" ] || find $(BOOT_FAT) -newer $(BOOT_STAMP) >/dev/null 2>&1; then \
		need_boot=1; \
	fi; \
	if [ ! -f "$(ROOT_STAMP)" ] || find $(ROOT_AXFS) -newer $(ROOT_STAMP) >/dev/null 2>&1; then \
		need_root=1; \
	fi; \
	if [ "$$need_boot" -eq 1 ]; then \
		printf '%s\n' 'disk.img: overlay BOOT partition'; \
		dd if=$(BOOT_FAT) of="$$target" bs=512 seek=$(BOOT_PART_LBA) conv=notrunc 2>/dev/null; \
		touch "$(BOOT_STAMP)"; \
	fi; \
	if [ "$$need_root" -eq 1 ]; then \
		printf '%s\n' 'disk.img: overlay ROOT partition'; \
		dd if=$(ROOT_AXFS) of="$$target" bs=512 seek=$(ROOT_PART_LBA) conv=notrunc 2>/dev/null; \
		touch "$(ROOT_STAMP)"; \
	fi; \
	touch "$$target"

# Regenerate the embedded bootsplash masks from resources/ (checked in, so a
# normal build does not need to run this).
include/bootsplash_assets.h: resources/axiome.svg tools/mkbootassets.py
	$(PYTHON) tools/mkbootassets.py

assets: include/bootsplash_assets.h

# Convenience alias so `make disk.img` / `make run` / `make install` work.
disk.img: $(DISK_PATH)

# Default boot: the axboot UEFI loader on disk.img, straight into OVMF.
# No CD-ROM needed. `run-iso` keeps the ISO path for hardware testing.
run: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) \
		-device $(QEMU_GPU_DEVICE) \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

# Linux-like 2D fallback (no virgl, like DRM_VIRTIO_GPU_KMS=n or VIRGL not offered)
run-2d: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY_2D) \
		-device $(QEMU_GPU_2D) \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

# Explicit 3D alias (same as default run, for symmetry)
run-3d: run

# Venus/GFXStream blob path (Linux virtio_gpu_object.c blob + host_visible_mm)
# Requires QEMU 6.0+ with -object memory-backend-memfd and hostmem
run-venus: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display sdl,gl=on \
		-device virtio-gpu-gl-pci,hostmem=4G,blob=true,venus=true \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

run-iso: iso disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-cdrom $(BUILD_DIR)/axiome.iso \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) \
		-device $(QEMU_GPU_DEVICE) \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

run-fb: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) -device $(QEMU_GPU_DEVICE)

run-usb: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) \
		-device $(QEMU_GPU_DEVICE) \
		-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

# ---------------------------------------------------------------------------
# Host-side unit tests.  Each test compiles the real kernel/userspace sources
# (never copies of them) against the host toolchain and runs as a native
# binary, so pure-logic units are exercised without booting the OS.
# ---------------------------------------------------------------------------
# The libc tests deliberately compare pointers returned by str*/mem* against
# the addresses of string literals, which clang diagnoses as
# -Wstring-compare / -Wstring-plus-int. GCC ignores these flags.
TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin
TEST_CFLAGS += -Wno-string-compare -Wno-string-plus-int
# macOS marks sprintf deprecated, but libc_stdio_test exercises it on purpose.
TEST_CFLAGS += -Wno-deprecated-declarations
KERNEL_INC  := -I$(REPO_ROOT)/kernel
# Prefer the freestanding headers only for quoted includes. Angle includes
# (<stdio.h> in test_util.h) keep using the host headers, so
# fprintf(stderr, ...) binds to the host libc instead of the axiome FILE.
LIBC_INC    := -iquote $(REPO_ROOT)/userspace/libc
LIBC_STUBS  := tests/libc_stubs_host.c

KERNEL_TESTS := \
	$(BUILD_DIR)/tests/hid_boot_test \
	$(BUILD_DIR)/tests/kernel_string_test \
	$(BUILD_DIR)/tests/kernel_net_util_test \
	$(BUILD_DIR)/tests/kernel_layout_test \
	$(BUILD_DIR)/tests/dynlink_regress_test

LIBC_TESTS := \
	$(BUILD_DIR)/tests/libc_string_test \
	$(BUILD_DIR)/tests/libc_stdlib_test \
	$(BUILD_DIR)/tests/libc_time_test \
	$(BUILD_DIR)/tests/libc_stdio_test

TEST_BINS := $(KERNEL_TESTS) $(LIBC_TESTS) $(BUILD_DIR)/tests/gfx_clip_test $(BUILD_DIR)/tests/input_abi_test $(BUILD_DIR)/tests/wm_abi_test $(BUILD_DIR)/tests/virtio_gpu_test

.PHONY: test test-hid

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t =="; $$t; done; \
	echo "All tests passed."

$(BUILD_DIR)/tests/hid_boot_test: tests/hid_boot_test.c kernel/hid_boot.c
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $^

# Native (host) C++ binary: must use HOSTCXX, not the cross CXX.
$(BUILD_DIR)/tests/gfx_clip_test: tests/gfx_clip_test.c kernel/gfx/gfx_types.h
	@mkdir -p $(@D)
	$(HOSTCXX) -x c++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-builtin $(KERNEL_INC) -o $@ $<

$(BUILD_DIR)/tests/input_abi_test: tests/input_abi_test.c kernel/input_abi.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<

$(BUILD_DIR)/tests/wm_abi_test: tests/wm_abi_test.c kernel/wm_abi.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<

$(BUILD_DIR)/tests/kernel_string_test: tests/kernel_string_test.c kernel/string.c
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $^

$(BUILD_DIR)/tests/kernel_net_util_test: tests/kernel_net_util_test.c kernel/net_util.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<

$(BUILD_DIR)/tests/kernel_layout_test: tests/kernel_layout_test.c kernel/elf.h kernel/axiomefs.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<

# Dynamic-linker regression: build a real PIE executable + shared library, then
# run kernel/dynlink.c against them (raw buffers, load bias 0, no execution -
# the same contract as the in-kernel loader). The fixtures are only *parsed*,
# never executed, so they are built with the cross compiler. That keeps them
# ELF even when the host compiler emits Mach-O (macOS), and -nostdlib keeps
# them free of libc runtime dependencies (crt, gcc support libs,
# __gmon_start__...) that no axiome binary ever has.
$(BUILD_DIR)/tests/dynlink_fixture_lib.o: tests/dynlink_fixture_lib.c
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -fPIC -c -o $@ $<

$(BUILD_DIR)/tests/dynlink_fixture_lib.sl: $(BUILD_DIR)/tests/dynlink_fixture_lib.o
	$(LD) -shared -soname dynlink_fixture_lib.sl -o $@ $<

$(BUILD_DIR)/tests/dynlink_fixture_app.o: tests/dynlink_fixture_app.c
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -fPIC -c -o $@ $<

$(BUILD_DIR)/tests/dynlink_fixture_app: $(BUILD_DIR)/tests/dynlink_fixture_app.o \
		$(BUILD_DIR)/tests/dynlink_fixture_lib.sl
	$(LD) -pie -e main --hash-style=sysv -o $@ $< \
		$(BUILD_DIR)/tests/dynlink_fixture_lib.sl

$(BUILD_DIR)/tests/dynlink_regress_test: tests/dynlink_regress_test.c kernel/dynlink.c \
		$(BUILD_DIR)/tests/dynlink_fixture_app $(BUILD_DIR)/tests/dynlink_fixture_lib.sl
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ tests/dynlink_regress_test.c kernel/dynlink.c

$(BUILD_DIR)/tests/libc_string_test: tests/libc_string_test.c \
		userspace/libc/string.c userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_stdlib_test: tests/libc_stdlib_test.c \
		userspace/libc/string.c userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_time_test: tests/libc_time_test.c \
		userspace/libc/time.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_stdio_test: tests/libc_stdio_test.c \
		userspace/libc/stdio.c userspace/libc/string.c \
		userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) -Wno-unused-function $(LIBC_INC) -o $@ $^

test-hid: $(BUILD_DIR)/tests/hid_boot_test
	$(BUILD_DIR)/tests/hid_boot_test

debug: disk.img
	qemu-system-x86_64 $(OVMF_ARGS) \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) -device $(QEMU_GPU_DEVICE) -s -S

install: disk.img
	$(SUDO) $(CURDIR)/tools/install.sh $(DEV) $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)/isodir $(BUILD_DIR)/isowork $(BUILD_DIR)/boot.fat \
		$(BUILD_DIR)/esp.img $(BUILD_DIR)/root.axfs $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/axiome.iso userspace/libc.sl
	$(MAKE) -C kernel clean BUILD_DIR=$(BUILD_DIR)/kernel
	$(MAKE) -C bootloader clean

distclean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/tests/virtio_gpu_test: tests/virtio_gpu_test.c kernel/virtio_gpu.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<
