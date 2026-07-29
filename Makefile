SHELL := /bin/bash

ROOT := $(CURDIR)
BUILD := $(ROOT)/build
CONDA := $(ROOT)/.conda/bin
CXX := $(CONDA)/clang++
LLVM_READELF := $(CONDA)/llvm-readelf
RISCV_PREFIX ?= /home/me/riscv/bin/riscv32-unknown-linux-gnu-
LD := $(RISCV_PREFIX)ld

RV_FLAGS := --target=riscv32-unknown-elf -march=rv32ima_zicsr -mabi=ilp32 \
	-mcmodel=medany -msmall-data-limit=0 -std=c++2c -ffreestanding \
	-fno-exceptions -fno-rtti -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -fno-threadsafe-statics \
	-fno-use-cxa-atexit -fno-stack-protector -fdata-sections \
	-ffunction-sections -Wall -Wextra -Werror -O2 -g \
	-I$(ROOT)/include -MMD -MP

KERNEL_ELF := $(BUILD)/mikos-rv32.elf
KERNEL_MAP := $(BUILD)/mikos-rv32.map
KERNEL_SOURCES := \
	kernel/main.cpp \
	kernel/runtime.cpp \
	kernel/syscall.cpp \
	drivers/net/virtio_mmio.cpp \
	network/stack.cpp \
	kernel/arch/riscv32/timer.cpp \
	kernel/arch/riscv32/entry.S \
	kernel/arch/riscv32/trap.S
KERNEL_OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(filter %.cpp,$(KERNEL_SOURCES))) \
	$(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(KERNEL_SOURCES)))
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)

# The current RV32 acceptance image embeds its user-space test workloads. Their
# source acquisition and build rules live with the BusyBox tests, not here.
BUSYBOX_TEST_BUILD := $(BUILD)/tests/busybox
BUSYBOX_BLOB := $(BUSYBOX_TEST_BUILD)/busybox_blob.o
STRESS_NG_BLOB := $(BUSYBOX_TEST_BUILD)/stress_ng_blob.o

QEMU := $(BUILD)/qemu/qemu-system-riscv32
NET_PEER := $(BUILD)/tests/qemu/net_peer
ETHGIG_TAP := $(BUILD)/tests/qemu/ethgig_tap

.PHONY: all test kernel inspect busybox stress-ng run qemu-test \
	qemu-net-test qemu-ssh-top ethgig-tap clean

all: test kernel

test:
	$(MAKE) -C tests test

kernel: $(KERNEL_ELF)

inspect: $(KERNEL_ELF)
	tests/kernel/inspect_kernel.sh

busybox:
	$(MAKE) -C tests/busybox busybox

stress-ng:
	$(MAKE) -C tests/busybox stress-ng

$(BUSYBOX_BLOB) $(STRESS_NG_BLOB) &: tests/busybox/Makefile \
		tests/busybox/config/busybox.config \
		tests/busybox/download_busybox.sh \
		tests/busybox/build_busybox.sh \
		tests/busybox/patches/stress-ng-mikos.patch
	$(MAKE) -C tests/busybox payloads

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(RV_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CXX) $(RV_FLAGS) -x assembler-with-cpp -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJECTS) $(BUSYBOX_BLOB) $(STRESS_NG_BLOB) \
		kernel/arch/riscv32/linker.ld
	$(LD) -m elf32lriscv -nostdlib --gc-sections \
		-T kernel/arch/riscv32/linker.ld \
		-Map $(KERNEL_MAP) -o $@ $(KERNEL_OBJECTS) $(BUSYBOX_BLOB) \
		$(STRESS_NG_BLOB)
	$(LLVM_READELF) -h -l $@ > $(BUILD)/mikos-rv32.headers.txt

run: $(KERNEL_ELF) $(QEMU)
	tests/qemu/run.sh

qemu-test: inspect $(QEMU)
	tests/qemu/run_qemu.sh

qemu-net-test: inspect $(NET_PEER) $(QEMU)
	tests/qemu/run_qemu_net.sh

qemu-ssh-top: ethgig-tap
	tests/qemu/run_qemu_ssh.sh

$(NET_PEER): tests/qemu/net_peer.cpp include/mikos/net/ethernet.hpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I$(ROOT)/include $< -o $@

ethgig-tap: $(ETHGIG_TAP)

$(ETHGIG_TAP): tests/qemu/ethgig_tap.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -Wall -Wextra -Werror $< -o $@

$(QEMU):
	@echo "QEMU is not built. See docs/poc-rv32.md." >&2
	@false

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C tests/busybox clean
	rm -f $(KERNEL_OBJECTS) $(KERNEL_DEPS) $(KERNEL_ELF) $(KERNEL_MAP) \
		$(BUILD)/mikos-rv32.headers.txt $(BUILD)/mikos-rv32.sections.txt \
		$(BUILD)/mikos-rv32.undefined.txt $(NET_PEER) $(ETHGIG_TAP)

-include $(KERNEL_DEPS)
