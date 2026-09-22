REPO_ROOT := $(realpath .)
BUILD_DIR := $(REPO_ROOT)/build

PYTHON := python3
HOSTCC := gcc

DEV ?= /dev/sdx

# QEMU 3D: virtio-gpu VirGL. Requires host virglrenderer + GL display (Linux-like).
# See ~/linux/drivers/gpu/drm/virtio/virtgpu_drv.c:features[] + virtio_has_feature().
# Default is 3D (virgl) like Linux DRM_VIRTIO_GPU_KMS; for 2D fallback:
#   make run-2d                # sdl + virtio-vga (no virgl, no gl)
#   make run QEMU_DISPLAY=sdl QEMU_GPU_DEVICE=virtio-vga
# For Venus/GFXStream (blob): make run QEMU_GPU_DEVICE="virtio-gpu-gl-pci,hostmem=4G,blob=true"
QEMU_DISPLAY ?= sdl,gl=on
QEMU_GPU_DEVICE ?= virtio-vga-gl
# Linux-like aliases: discrete vs integrated VGA variant
QEMU_GPU_2D ?= virtio-vga
QEMU_DISPLAY_2D ?= sdl

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

.PHONY: all kernel bootloader iso run run-2d run-3d run-venus run-iso run-fb run-usb debug test test-hid clean distclean install disk.img

all: iso

kernel:
	$(MAKE) -C kernel BUILD_DIR=$(BUILD_DIR)/kernel

bootloader:
	$(MAKE) -C bootloader

# Host-side axiomefs formatter (enhanced to take a disk image + offset).
$(BUILD_DIR)/kernel/mkfs_axiomefs: tools/mkfs_axiomefs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -I $(REPO_ROOT) -o $@ $<

# The UEFI loader binary. Forward to the bootloader sub-make when it is
# missing or its sources changed.
$(BUILD_DIR)/bootloader/BOOTX64.EFI: $(wildcard bootloader/*.c bootloader/*.h bootloader/Makefile include/axboot.h)
	$(MAKE) -C bootloader

iso: kernel bootloader $(BOOT_FAT)
	rm -rf $(BUILD_DIR)/isowork
	mkdir -p $(BUILD_DIR)/isowork
	cp $(BOOT_FAT) $(BUILD_DIR)/isowork/esp.img
	xorriso -as mkisofs -R -f -e esp.img -no-emul-boot \
		-o $(BUILD_DIR)/axiome.iso $(BUILD_DIR)/isowork 2>/dev/null

# BOOT partition: a FAT32 ESP image holding the UEFI bootloader and the
# kernel. Rebuilt when either of them changes. The bootloader reads
# \kernel.elf from the volume root via SimpleFileSystem.
$(BOOT_FAT): $(BUILD_DIR)/kernel/kernel.elf $(BUILD_DIR)/bootloader/BOOTX64.EFI
	@mkdir -p $(dir $@)
	rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=$(BOOT_PART_SECTORS) 2>/dev/null
	mformat -F -i $@ -v BOOT ::
	mcopy -i $@ $(BUILD_DIR)/kernel/kernel.elf ::kernel.elf
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
$(DISK_PATH): kernel bootloader $(BUILD_DIR)/kernel/mkfs_axiomefs $(BOOT_FAT) $(ROOT_AXFS) tools/mkpart.py
	@mkdir -p $(BUILD_DIR)
	@target=$(DISK_PATH); \
	if [ ! -f "$$target" ] || [ tools/mkpart.py -nt "$$target" ]; then \
		printf '%s\n' 'disk.img: creating fresh image + MBR'; \
		dd if=/dev/zero of="$$target" bs=512 count=$(DISK_SECTORS) 2>/dev/null; \
		$(PYTHON) tools/mkpart.py "$$target"; \
		need_boot=1; need_root=1; \
	else \
		need_boot=0; need_root=0; \
	fi; \
	if [ ! -f "$(BOOT_STAMP)" ] || [ $(BOOT_FAT) -nt "$(BOOT_STAMP)" ]; then \
		need_boot=1; \
	fi; \
	if [ ! -f "$(ROOT_STAMP)" ] || [ $(ROOT_AXFS) -nt "$(ROOT_STAMP)" ]; then \
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

# Convenience alias so `make disk.img` / `make run` / `make install` work.
disk.img: $(DISK_PATH)

# Default boot: the axboot UEFI loader on disk.img, straight into OVMF.
# No CD-ROM needed. `run-iso` keeps the ISO path for hardware testing.
run: disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) \
		-device $(QEMU_GPU_DEVICE) \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

# Linux-like 2D fallback (no virgl, like DRM_VIRTIO_GPU_KMS=n or VIRGL not offered)
run-2d: disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
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
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display sdl,gl=on \
		-device virtio-gpu-gl-pci,hostmem=4G,blob=true,venus=true \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

run-iso: iso disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(BUILD_DIR)/axiome.iso \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) \
		-device $(QEMU_GPU_DEVICE) \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

run-fb: disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) -device $(QEMU_GPU_DEVICE)

run-usb: disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
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
TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin
KERNEL_INC  := -I$(REPO_ROOT)/kernel
LIBC_INC    := -I$(REPO_ROOT)/kernel/userspace/libc
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

TEST_BINS := $(KERNEL_TESTS) $(LIBC_TESTS) $(BUILD_DIR)/tests/gfx_clip_test $(BUILD_DIR)/tests/axdri_cmd_test $(BUILD_DIR)/tests/input_abi_test $(BUILD_DIR)/tests/wm_abi_test $(BUILD_DIR)/tests/virtio_gpu_test

.PHONY: test test-hid

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t =="; $$t; done; \
	echo "All tests passed."

$(BUILD_DIR)/tests/hid_boot_test: tests/hid_boot_test.c kernel/hid_boot.c
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $^

$(BUILD_DIR)/tests/gfx_clip_test: tests/gfx_clip_test.c kernel/gfx/gfx_types.h
	@mkdir -p $(@D)
	g++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-builtin $(KERNEL_INC) -o $@ $<

$(BUILD_DIR)/tests/axdri_cmd_test: tests/axdri_cmd_test.c kernel/axdri_cmd.h ports/mesa-axiome/axdri.c ports/mesa-axiome/axdri.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) -DAXDRI_HOST_TEST -DAXDRI_KERNEL_HEADERS -I$(REPO_ROOT) $(KERNEL_INC) -o $@ $<

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

# Dynamic-linker regression: build a real PIE executable + shared library with
# the host toolchain, then run kernel/dynlink.c against them (raw buffers,
# load bias 0, no execution - the same contract as the in-kernel loader).
# -nostdlib keeps the images free of libc runtime dependencies (crt, gcc
# support libs, __gmon_start__...) that no axiome binary ever has.
$(BUILD_DIR)/tests/dynlink_fixture_lib.sl: tests/dynlink_fixture_lib.c
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) -fPIC -shared -nostdlib \
		-Wl,-soname,dynlink_fixture_lib.sl -o $@ $<

$(BUILD_DIR)/tests/dynlink_fixture_app: tests/dynlink_fixture_app.c \
		$(BUILD_DIR)/tests/dynlink_fixture_lib.sl
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) -fPIC -pie -nostdlib -Wl,--hash-style=sysv \
		-Wl,-e,main -o $@ $< $(BUILD_DIR)/tests/dynlink_fixture_lib.sl

$(BUILD_DIR)/tests/dynlink_regress_test: tests/dynlink_regress_test.c kernel/dynlink.c \
		$(BUILD_DIR)/tests/dynlink_fixture_app $(BUILD_DIR)/tests/dynlink_fixture_lib.sl
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ tests/dynlink_regress_test.c kernel/dynlink.c

$(BUILD_DIR)/tests/libc_string_test: tests/libc_string_test.c \
		kernel/userspace/libc/string.c kernel/userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_stdlib_test: tests/libc_stdlib_test.c \
		kernel/userspace/libc/string.c kernel/userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_time_test: tests/libc_time_test.c \
		kernel/userspace/libc/time.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(LIBC_INC) -o $@ $^

$(BUILD_DIR)/tests/libc_stdio_test: tests/libc_stdio_test.c \
		kernel/userspace/libc/stdio.c kernel/userspace/libc/string.c \
		kernel/userspace/libc/stdlib.c $(LIBC_STUBS)
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) -Wno-unused-function $(LIBC_INC) -o $@ $^

test-hid: $(BUILD_DIR)/tests/hid_boot_test
	$(BUILD_DIR)/tests/hid_boot_test

debug: disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0,media=disk \
		-m 512M -serial stdio -display $(QEMU_DISPLAY) -device $(QEMU_GPU_DEVICE) -s -S

install: disk.img
	sudo $(shell pwd)/tools/install.sh $(DEV) $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)/isodir $(BUILD_DIR)/isowork $(BUILD_DIR)/boot.fat \
		$(BUILD_DIR)/esp.img $(BUILD_DIR)/root.axfs $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/axiome.iso
	$(MAKE) -C kernel clean BUILD_DIR=$(BUILD_DIR)/kernel
	$(MAKE) -C bootloader clean

distclean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/tests/virtio_gpu_test: tests/virtio_gpu_test.c kernel/virtio_gpu.h
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_CFLAGS) $(KERNEL_INC) -o $@ $<
