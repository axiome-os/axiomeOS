REPO_ROOT := $(realpath ..)
BUILD_DIR := $(REPO_ROOT)/build/kernel

CC      := $(HOME)/opt/cross/bin/x86_64-elf-gcc
CXX     := $(HOME)/opt/cross/bin/x86_64-elf-g++
AS      := $(HOME)/opt/cross/bin/x86_64-elf-as
LD      := $(HOME)/opt/cross/bin/x86_64-elf-ld

CFLAGS  = -ffreestanding -nostdlib -nostartfiles
CFLAGS += -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2
CFLAGS += -O2 -g -pipe
CFLAGS += -Wall -Wextra -Wpedantic
CFLAGS += -I$(REPO_ROOT) -I$(REPO_ROOT)/kernel -I$(REPO_ROOT)/include
CFLAGS += -DGIT_COMMIT=\"$(shell git rev-parse --short HEAD)\"
CFLAGS += -DBUILD_TIME=\"$(shell date +%s)\"
CFLAGS += -DBUILD_TIME_UNIX=$(shell date +%s)ULL
CFLAGS += -DBUILD_TZ_OFFSET_SEC=$(shell python3 -c "import time; print(-time.timezone if not time.daylight else -time.altzone)")LL

# C++ is used for the hardware abstraction layer. Exceptions/RTTI and the
# standard library are unavailable in the kernel; placement new into static
# storage means no .init_array support is required.
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics
CXXFLAGS += -fno-use-cxa-atexit -fno-unwind-tables -fno-asynchronous-unwind-tables

LDFLAGS = -nostdlib -T $(REPO_ROOT)/linker.ld

C_SRCS := $(wildcard *.c) $(wildcard arch/x86_64/*.c)
# Built-in storage drivers: their sources live under modules/ but they link
# into kernel.elf (not .kxt) and register via driver_init(). The define
# switches their packaging tail from MODULE_INIT to *_driver_init().
C_SRCS += modules/sata.c modules/nvme.c
$(BUILD_DIR)/modules/sata.o $(BUILD_DIR)/modules/nvme.o: CFLAGS += -DAXIOME_BUILTIN_DRIVER
CXX_SRCS := $(wildcard hal/*.cpp) $(wildcard arch/x86_64/hal/*.cpp) $(wildcard gfx/*.cpp)
ASM_SRCS := $(wildcard arch/x86_64/*.S)
C_OBJS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS))
CXX_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(CXX_SRCS))
ASM_OBJS := $(patsubst %.S, $(BUILD_DIR)/%.o, $(ASM_SRCS))

# Userspace programs. They are built into the source tree as .elf and shipped
# on the root partition (root_manifest.txt), then loaded from disk at runtime
# by the ELF loader. They are deliberately NOT embedded in the kernel image.
USER_PROGS := init hello cat echo sh ls mkdir cp mv rm touch whoami uname \
	login su chmod chown vfs_test ipc_test proc_test driver_test net_test \
	kill kxtload kxtunload 	syslogd oobe date burn forkt ping gfx_test \
	axwm axterm evtest axinfo guixd axlogin axoobe
USER_ELFS := $(addprefix userspace/,$(addsuffix .elf,$(USER_PROGS)))

# Every user binary is a PIE that depends on libc.sl (shared library).
# crt0.o stays statically linked so each executable has its own _start.
# libc.sl and userspace/link.ld fix the load bases (0x8000000000 and
# 0x10000000000) that the in-kernel dynamic linker relies on.
USER_BASE_CFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	-fno-stack-protector -fPIC -mcmodel=small -O2 -g -pipe \
	-mno-red-zone -Wall -Wextra -I userspace/libc
USER_CFLAGS := $(USER_BASE_CFLAGS) -fpie

# libc.sl is a real shared object: every global symbol is preemptible, so it
# must be built as plain PIC (never -fpie, which relaxes same-TU references
# and also happens to clobber -fPIC in newer gcc).
LIBC_CFLAGS := $(USER_BASE_CFLAGS)

CRT0_OBJ := $(BUILD_DIR)/user_crt0.o

LIBC_SRCS := userspace/libc/errno.c userspace/libc/string.c \
	userspace/libc/stdlib.c userspace/libc/stdio.c \
	userspace/libc/syscall.c userspace/libc/signal.c \
	userspace/libc/time.c
LIBC_OBJS := $(patsubst userspace/libc/%.c, $(BUILD_DIR)/libc_%.o, $(LIBC_SRCS))
LIBC_SL := userspace/libc.sl

.PHONY: all clean modules

all: $(BUILD_DIR)/kernel.elf modules $(USER_ELFS)

# Build loadable .kxt modules (e1000, hello, ...) into the source tree so the
# disk image builder (mkfs_axiomefs) can embed them under /System/Extensions.
modules:
	$(MAKE) -C modules

$(BUILD_DIR)/kernel.elf: $(ASM_OBJS) $(C_OBJS) $(CXX_OBJS)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $^ -z max-page-size=0x1000

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(AS) -c -o $@ $<

$(BUILD_DIR)/%_user.o: userspace/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/user_crt0.o: userspace/libc/crt0.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_%.o: userspace/libc/%.c
	@mkdir -p $(@D)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

# Shared C library: every user binary links against this instead of dragging
# its own static copy. Built with a fixed base so the kernel can map it at
# its link-time address (load bias 0) and resolve RELA directly.
$(LIBC_SL): $(LIBC_OBJS) userspace/libc.ld
	@mkdir -p $(@D)
	$(LD) -shared -T userspace/libc.ld --hash-style=sysv \
		-soname libc.sl -z max-page-size=0x1000 -o $@ $(LIBC_OBJS)

# PIE user executable, dynamically linked against libc.sl (DT_NEEDED picked
# up by link.ld; resolved at runtime by kernel/elf.c + kernel/dynlink.c).
userspace/%.elf: $(BUILD_DIR)/%_user.o $(CRT0_OBJ) $(LIBC_SL)
	@mkdir -p $(@D)
	$(LD) -pie -T userspace/link.ld --hash-style=sysv \
		-z max-page-size=0x1000 -e _start -o $@ $^

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C modules clean

# Auto-generated header dependencies (see -MMD -MP on the compile rules
# above): a header edit rebuilds every object that includes it, so struct
# layout changes can never again leave stale .o files behind.
-include $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)
