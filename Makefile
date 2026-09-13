SHELL := /bin/bash

ROOT := $(CURDIR)
BUILD := $(ROOT)/build
CONDA := $(ROOT)/.conda/bin
CXX := $(CONDA)/clang++
LLVM_READELF := $(CONDA)/llvm-readelf
include support/toolchain.mk
LD := $(RISCV_PREFIX)ld
OBJCOPY := $(RISCV_PREFIX)objcopy

.DEFAULT_GOAL := all
include support/kernel-cxx/flags.mk

RV_FLAGS = --target=riscv32-unknown-elf -march=rv32ima_zicsr -mabi=ilp32 \
	-mcmodel=medany -msmall-data-limit=0 -std=c++2c -ffreestanding \
	-fno-exceptions -fno-rtti -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -fno-threadsafe-statics \
	-fno-use-cxa-atexit -fno-stack-protector -fdata-sections \
	-ffunction-sections -Wall -Wextra -Werror -O2 -g \
	-I$(ROOT) -I$(ROOT)/include -MMD -MP $(KERNEL_CXX_TARGET_HEADERS)

KERNEL_ELF := $(BUILD)/mikos-rv32.elf
KERNEL_MAP := $(BUILD)/mikos-rv32.map
KERNEL_SOURCES := \
	kernel/main.cpp \
	kernel/runtime.cpp \
	kernel/cxx_arena.cpp \
	kernel/cxx_runtime.cpp \
	kernel/cxx_math.cpp \
	kernel/cxx_smoke.cpp \
	kernel/syscall.cpp \
	kernel/pseudo_filesystem.cpp \
	drivers/uart/ns16550a.cpp \
	drivers/net/virtio_mmio.cpp \
	drivers/storage/virtio_block.cpp \
	drivers/storage/root_device.cpp \
	drivers/fs/root.cpp \
	network/stack.cpp \
	kernel/arch/riscv32/timer.cpp \
	kernel/arch/riscv32/entry.S \
	kernel/arch/riscv32/trap.S
KERNEL_OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(filter %.cpp,$(KERNEL_SOURCES))) \
	$(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(KERNEL_SOURCES)))
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)

TRIBE_RV_FLAGS = $(subst -march=rv32ima_zicsr,-march=rv32im_zicsr,$(RV_FLAGS)) \
	-DMIKOS_TRIBE
TRIBE_KERNEL_ELF := $(BUILD)/mikos-tribe-rv32.elf
TRIBE_KERNEL_MAP := $(BUILD)/mikos-tribe-rv32.map
TRIBE_KERNEL_SOURCES := \
	kernel/main.cpp \
	kernel/runtime.cpp \
	kernel/cxx_arena.cpp \
	kernel/cxx_runtime.cpp \
	kernel/cxx_math.cpp \
	kernel/cxx_smoke.cpp \
	kernel/syscall.cpp \
	kernel/pseudo_filesystem.cpp \
	drivers/uart/ns16550a.cpp \
	drivers/net/tribe_ethgig.cpp \
	drivers/storage/tribe_sd.cpp \
	drivers/storage/root_device.cpp \
	drivers/fs/root.cpp \
	network/stack.cpp \
	kernel/arch/riscv32/timer.cpp \
	kernel/arch/riscv32/entry.S \
	kernel/arch/riscv32/trap.S
TRIBE_KERNEL_OBJECTS := \
	$(patsubst %.cpp,$(BUILD)/tribe/%.o,$(filter %.cpp,$(TRIBE_KERNEL_SOURCES))) \
	$(patsubst %.S,$(BUILD)/tribe/%.o,$(filter %.S,$(TRIBE_KERNEL_SOURCES)))
TRIBE_KERNEL_DEPS := $(TRIBE_KERNEL_OBJECTS:.o=.d)
TRIBE_INTERACTIVE_RV_FLAGS = $(TRIBE_RV_FLAGS) -DMIKOS_TRIBE_INTERACTIVE
TRIBE_INTERACTIVE_ELF := $(BUILD)/mikos-tribe-interactive-rv32.elf
TRIBE_INTERACTIVE_MAP := $(BUILD)/mikos-tribe-interactive-rv32.map
TRIBE_INTERACTIVE_OBJECTS := \
	$(patsubst %.cpp,$(BUILD)/tribe-interactive/%.o,$(filter %.cpp,$(TRIBE_KERNEL_SOURCES))) \
	$(patsubst %.S,$(BUILD)/tribe-interactive/%.o,$(filter %.S,$(TRIBE_KERNEL_SOURCES)))
TRIBE_INTERACTIVE_DEPS := $(TRIBE_INTERACTIVE_OBJECTS:.o=.d)
TRIBE_INTERACTIVE_MULTICORE_RV_FLAGS = $(TRIBE_INTERACTIVE_RV_FLAGS) \
	-DMIKOS_TRIBE_MULTICORE
TRIBE_INTERACTIVE_MULTICORE_ELF := \
	$(BUILD)/mikos-tribe-interactive-multicore-rv32.elf
TRIBE_INTERACTIVE_MULTICORE_MAP := \
	$(BUILD)/mikos-tribe-interactive-multicore-rv32.map
TRIBE_INTERACTIVE_MULTICORE_OBJECTS := \
	$(patsubst %.cpp,$(BUILD)/tribe-interactive-multicore/%.o,$(filter %.cpp,$(TRIBE_KERNEL_SOURCES))) \
	$(patsubst %.S,$(BUILD)/tribe-interactive-multicore/%.o,$(filter %.S,$(TRIBE_KERNEL_SOURCES)))
TRIBE_INTERACTIVE_MULTICORE_DEPS := \
	$(TRIBE_INTERACTIVE_MULTICORE_OBJECTS:.o=.d)

KERNEL_OBJECTS += $(BUILD)/kernel/cxx_hash.o
TRIBE_KERNEL_OBJECTS += $(BUILD)/tribe/kernel/cxx_hash.o
TRIBE_INTERACTIVE_OBJECTS += $(BUILD)/tribe-interactive/kernel/cxx_hash.o
TRIBE_INTERACTIVE_MULTICORE_OBJECTS += $(BUILD)/tribe-interactive-multicore/kernel/cxx_hash.o

# std::unordered_set uses float load-factor arithmetic. Compile only the five
# integer-only compiler-rt leaf implementations it needs; no FPU/libm/unwinder.
KERNEL_SOFTFLOAT_NAMES := comparesf2 divsf3 mulsf3 fixunssfsi floatunsisf
KERNEL_SOFTFLOAT_OBJECTS := $(addprefix $(BUILD)/kernel/softfloat/,$(addsuffix .o,$(KERNEL_SOFTFLOAT_NAMES)))
KERNEL_OBJECTS += $(KERNEL_SOFTFLOAT_OBJECTS)
TRIBE_KERNEL_OBJECTS += $(KERNEL_SOFTFLOAT_OBJECTS)
TRIBE_INTERACTIVE_OBJECTS += $(KERNEL_SOFTFLOAT_OBJECTS)
TRIBE_INTERACTIVE_MULTICORE_OBJECTS += $(KERNEL_SOFTFLOAT_OBJECTS)

$(BUILD)/kernel/softfloat/%.o: $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(filter-out -std=c++2c,$(TRIBE_RV_FLAGS)) -x c -std=c11 \
	  -c $(KERNEL_CXX_BUILTINS)/$*.c -o $@

$(KERNEL_OBJECTS) $(TRIBE_KERNEL_OBJECTS) $(TRIBE_INTERACTIVE_OBJECTS) \
  $(TRIBE_INTERACTIVE_MULTICORE_OBJECTS): Makefile support/toolchain.mk support/kernel-cxx/flags.mk $(KERNEL_CXX_READY)

$(BUILD)/kernel/cxx_hash.o: $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(RV_FLAGS) -c $(KERNEL_CXX_HASH) -o $@
$(BUILD)/tribe/kernel/cxx_hash.o: $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_RV_FLAGS) -c $(KERNEL_CXX_HASH) -o $@
$(BUILD)/tribe-interactive/kernel/cxx_hash.o: $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_RV_FLAGS) -c $(KERNEL_CXX_HASH) -o $@
$(BUILD)/tribe-interactive-multicore/kernel/cxx_hash.o: $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_MULTICORE_RV_FLAGS) -c $(KERNEL_CXX_HASH) -o $@

# BusyBox and stress-ng live in one ext4 root image shared by QEMU and Tribe.
BUSYBOX_TEST_BUILD := $(BUILD)/tests/busybox
ROOTFS_IMAGE := $(BUSYBOX_TEST_BUILD)/rootfs.ext4
BUSYBOX_BINARY := $(BUSYBOX_TEST_BUILD)/busybox/busybox
DROPBEAR_BINARY := $(BUSYBOX_TEST_BUILD)/dropbear-source/dropbear
TRIBE_EMBEDDED_BUSYBOX_OBJECT := \
	$(BUILD)/tribe-interactive/embedded-busybox.o
TRIBE_EMBEDDED_DROPBEAR_OBJECT := \
	$(BUILD)/tribe-interactive/embedded-dropbear.o

QEMU := $(BUILD)/qemu/qemu-system-riscv32
NET_PEER := $(BUILD)/tests/qemu/net_peer
ETHGIG_TAP := $(BUILD)/tests/qemu/ethgig_tap
TRIBE_NET_PEER := $(BUILD)/tests/tribe/net_peer
CPPHDL_HOME ?=
export CPPHDL_HOME
CPPHDL_TAP := $(BUILD)/tests/tribe/ethgig_tap

.PHONY: all test kernel image tribe-kernel tribe-interactive-kernel \
	tribe-interactive-multicore-kernel inspect busybox dropbear-client \
	stress-ng run qemu-test qemu-net-test qemu-ssh-top tribe-prepare tribe-test \
	tribe-interactive tribe-interactive-ping-test \
	tribe-interactive-tcp-test tribe-interactive-process-test \
	tribe-interactive-ssh-test ethgig-tap clean
.PHONY: tribe-boot-test tribe-kernel-test tribe-all-tests tribe-tap tribe-peer-test

all: test kernel

test: tribe-peer-test
	$(MAKE) -C tests test

kernel: $(KERNEL_ELF)

image: $(ROOTFS_IMAGE) $(KERNEL_ELF)

tribe-kernel: $(TRIBE_KERNEL_ELF)

tribe-interactive-kernel: $(ROOTFS_IMAGE) $(TRIBE_INTERACTIVE_ELF)

tribe-interactive-multicore-kernel: $(ROOTFS_IMAGE) \
	$(TRIBE_INTERACTIVE_MULTICORE_ELF)

inspect: $(KERNEL_ELF)
	tests/kernel/inspect_kernel.sh

.PHONY: kernel-cxx-test
kernel-cxx-test: $(KERNEL_ELF) $(TRIBE_KERNEL_ELF) \
    $(TRIBE_INTERACTIVE_ELF) $(TRIBE_INTERACTIVE_MULTICORE_ELF)
	$(MAKE) -C tests kernel-cxx-check
	@for elf in $^; do tests/kernel/inspect_kernel.sh "$$elf"; done
	bash tests/qemu/run_kernel_cxx.sh

busybox:
	$(MAKE) -C tests/busybox all

.PHONY: userspace-toolchain
userspace-toolchain:
	RISCV_HOME='$(RISCV_HOME)' RISCV_USERSPACE_HOME='$(RISCV_USERSPACE_HOME)' \
		bash tests/busybox/build_toolchain.sh

dropbear-client:
	$(MAKE) -C tests/busybox dropbear-client

stress-ng:
	$(MAKE) -C tests/busybox stress-ng

$(ROOTFS_IMAGE): tests/busybox/Makefile \
		tests/busybox/config/busybox.config \
		tests/busybox/config/dropbear_localoptions.h \
		tests/busybox/download_busybox.sh \
		tests/busybox/build_busybox.sh \
		tests/busybox/download_dropbear.sh \
		tests/busybox/build_dropbear.sh \
		tests/busybox/patches/dropbear-mikos.patch \
		tests/busybox/rootfs/etc/group \
		tests/busybox/rootfs/etc/inittab \
		tests/busybox/rootfs/etc/passwd \
		tests/busybox/rootfs/etc/profile \
		tests/busybox/rootfs/etc/shells \
		tests/busybox/rootfs/etc/init.d/rcS \
		tests/busybox/rootfs/etc/dropbear/dropbear_ed25519_host_key.b64 \
		tests/busybox/rootfs/root/.ssh/authorized_keys \
		tests/busybox/set_rootfs_owners.sh \
		tests/busybox/verify_rootfs.sh \
		tests/busybox/patches/stress-ng-mikos.patch
	$(MAKE) -C tests/busybox rootfs

$(TRIBE_EMBEDDED_BUSYBOX_OBJECT): $(ROOTFS_IMAGE)
	@mkdir -p $(@D)
	$(OBJCOPY) -I binary -O elf32-littleriscv -B riscv \
		--rename-section .data=.user_busybox,alloc,load,readonly,data,contents \
		$(BUSYBOX_BINARY) $@

$(TRIBE_EMBEDDED_DROPBEAR_OBJECT): $(ROOTFS_IMAGE)
	@mkdir -p $(@D)
	$(OBJCOPY) -I binary -O elf32-littleriscv -B riscv \
		--rename-section .data=.user_dropbear,alloc,load,readonly,data,contents \
		$(DROPBEAR_BINARY) $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(RV_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CXX) $(RV_FLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/tribe/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_RV_FLAGS) -c $< -o $@

$(BUILD)/tribe/%.o: %.S
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_RV_FLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/tribe-interactive/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_RV_FLAGS) -c $< -o $@

$(BUILD)/tribe-interactive/%.o: %.S
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_RV_FLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/tribe-interactive-multicore/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_MULTICORE_RV_FLAGS) -c $< -o $@

$(BUILD)/tribe-interactive-multicore/%.o: %.S
	@mkdir -p $(@D)
	$(CXX) $(TRIBE_INTERACTIVE_MULTICORE_RV_FLAGS) \
		-x assembler-with-cpp -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJECTS) \
		kernel/arch/riscv32/linker.ld
	$(LD) -m elf32lriscv -nostdlib --gc-sections \
		-T kernel/arch/riscv32/linker.ld \
		-Map $(KERNEL_MAP) -o $@ $(KERNEL_OBJECTS)
	$(LLVM_READELF) -h -l $@ > $(BUILD)/mikos-rv32.headers.txt

$(TRIBE_KERNEL_ELF): $(TRIBE_KERNEL_OBJECTS) \
		kernel/arch/riscv32/linker.ld
	$(LD) -m elf32lriscv -nostdlib --gc-sections \
		-T kernel/arch/riscv32/linker.ld \
		-Map $(TRIBE_KERNEL_MAP) -o $@ $(TRIBE_KERNEL_OBJECTS)
	$(LLVM_READELF) -h -l $@ > $(BUILD)/mikos-tribe-rv32.headers.txt

$(TRIBE_INTERACTIVE_ELF): $(TRIBE_INTERACTIVE_OBJECTS) \
		$(TRIBE_EMBEDDED_BUSYBOX_OBJECT) \
		$(TRIBE_EMBEDDED_DROPBEAR_OBJECT) \
		kernel/arch/riscv32/linker.ld
	$(LD) -m elf32lriscv -nostdlib --gc-sections \
		-T kernel/arch/riscv32/linker.ld \
		-Map $(TRIBE_INTERACTIVE_MAP) -o $@ \
		$(TRIBE_INTERACTIVE_OBJECTS) $(TRIBE_EMBEDDED_BUSYBOX_OBJECT) \
		$(TRIBE_EMBEDDED_DROPBEAR_OBJECT)
	$(LLVM_READELF) -h -l $@ > \
		$(BUILD)/mikos-tribe-interactive-rv32.headers.txt

$(TRIBE_INTERACTIVE_MULTICORE_ELF): \
		$(TRIBE_INTERACTIVE_MULTICORE_OBJECTS) \
		$(TRIBE_EMBEDDED_BUSYBOX_OBJECT) \
		$(TRIBE_EMBEDDED_DROPBEAR_OBJECT) \
		kernel/arch/riscv32/linker.ld
	$(LD) -m elf32lriscv -nostdlib --gc-sections \
		-T kernel/arch/riscv32/linker.ld \
		-Map $(TRIBE_INTERACTIVE_MULTICORE_MAP) -o $@ \
		$(TRIBE_INTERACTIVE_MULTICORE_OBJECTS) \
		$(TRIBE_EMBEDDED_BUSYBOX_OBJECT) \
		$(TRIBE_EMBEDDED_DROPBEAR_OBJECT)
	$(LLVM_READELF) -h -l $@ > \
		$(BUILD)/mikos-tribe-interactive-multicore-rv32.headers.txt

run: $(ROOTFS_IMAGE) $(KERNEL_ELF) $(QEMU)
	tests/qemu/run.sh

qemu-test: $(ROOTFS_IMAGE) inspect $(QEMU)
	tests/qemu/run_qemu.sh

qemu-net-test: $(ROOTFS_IMAGE) inspect $(NET_PEER) $(QEMU)
	tests/qemu/run_qemu_net.sh

tribe-prepare:
	tests/tribe/prepare_cpphdl.sh

tribe-peer-test: $(TRIBE_NET_PEER)
	python3 tests/tribe/test_net_peer.py $(TRIBE_NET_PEER)

tribe-kernel-test: tribe-prepare tribe-kernel $(TRIBE_NET_PEER)
	bash tests/tribe/run_kernel.sh

tribe-boot-test: tribe-prepare tribe-kernel
	bash tests/tribe/run_boot.sh

tribe-all-tests:
	bash tests/tribe/run_all.sh

tribe-tap:
	@test -f '$(CPPHDL_HOME)/tribe_cpu/linux/net/ethgig_tap.cpp' || { \
		echo 'Set CPPHDL_HOME to your current cpphdl checkout (export CPPHDL_HOME=$$HOME/cpphdl).' >&2; exit 1; }
	@mkdir -p $(dir $(CPPHDL_TAP))
	$(CXX) -std=c++2c -O2 -Wall -Wextra \
		'$(CPPHDL_HOME)/tribe_cpu/linux/net/ethgig_tap.cpp' -o $(CPPHDL_TAP)

tribe-test: tribe-prepare $(ROOTFS_IMAGE) $(TRIBE_KERNEL_ELF) $(TRIBE_NET_PEER)
	tests/tribe/run_tribe.sh

tribe-interactive: $(ROOTFS_IMAGE) $(TRIBE_INTERACTIVE_ELF)
	tests/tribe/tribe_interactive.sh

tribe-interactive-ping-test: $(ROOTFS_IMAGE) \
		$(TRIBE_INTERACTIVE_MULTICORE_ELF)
	tests/tribe/tribe_interactive.sh --multicore --test ping

tribe-interactive-tcp-test: $(ROOTFS_IMAGE) \
		$(TRIBE_INTERACTIVE_MULTICORE_ELF)
	tests/tribe/tribe_interactive.sh --multicore --test tcp

tribe-interactive-process-test: $(ROOTFS_IMAGE) \
		$(TRIBE_INTERACTIVE_MULTICORE_ELF)
	tests/tribe/tribe_interactive.sh --multicore --test process

tribe-interactive-ssh-test: $(ROOTFS_IMAGE) \
		$(TRIBE_INTERACTIVE_MULTICORE_ELF)
	tests/tribe/tribe_interactive.sh --multicore --test ssh

qemu-ssh-top: ethgig-tap
	tests/qemu/run_qemu_ssh.sh

$(NET_PEER): tests/qemu/net_peer.cpp include/mikos/net/ethernet.hpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I$(ROOT)/include $< -o $@

$(TRIBE_NET_PEER): tests/tribe/net_peer.cpp include/mikos/net/ethernet.hpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I$(ROOT)/include $< -o $@

ethgig-tap: $(ETHGIG_TAP)

$(ETHGIG_TAP): tests/qemu/ethgig_tap.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -Wall -Wextra -Werror $< -o $@

$(QEMU):
	@echo "QEMU is not built. See doc/poc-rv32.md." >&2
	@false

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C tests/busybox clean
	rm -f $(KERNEL_OBJECTS) $(KERNEL_DEPS) $(KERNEL_ELF) $(KERNEL_MAP) \
		$(TRIBE_KERNEL_OBJECTS) $(TRIBE_KERNEL_DEPS) $(TRIBE_KERNEL_ELF) \
		$(TRIBE_KERNEL_MAP) $(BUILD)/mikos-tribe-rv32.headers.txt \
		$(TRIBE_INTERACTIVE_OBJECTS) $(TRIBE_INTERACTIVE_DEPS) \
		$(TRIBE_INTERACTIVE_ELF) $(TRIBE_INTERACTIVE_MAP) \
		$(BUILD)/mikos-tribe-interactive-rv32.headers.txt \
		$(TRIBE_INTERACTIVE_MULTICORE_OBJECTS) \
		$(TRIBE_INTERACTIVE_MULTICORE_DEPS) \
		$(TRIBE_INTERACTIVE_MULTICORE_ELF) \
		$(TRIBE_INTERACTIVE_MULTICORE_MAP) \
		$(TRIBE_EMBEDDED_BUSYBOX_OBJECT) \
		$(TRIBE_EMBEDDED_DROPBEAR_OBJECT) \
		$(BUILD)/mikos-tribe-interactive-multicore-rv32.headers.txt \
		$(BUILD)/mikos-rv32.headers.txt $(BUILD)/mikos-rv32.sections.txt \
		$(BUILD)/mikos-rv32.undefined.txt $(NET_PEER) $(ETHGIG_TAP)
	@rm -f $(TRIBE_NET_PEER)

-include $(KERNEL_DEPS) $(TRIBE_KERNEL_DEPS) $(TRIBE_INTERACTIVE_DEPS) \
	$(TRIBE_INTERACTIVE_MULTICORE_DEPS)
