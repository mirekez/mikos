SHELL := /bin/bash

ROOT := $(abspath ..)
BUILD := $(ROOT)/build/tests/regression
CXX := $(ROOT)/.conda/bin/clang++
CPPFLAGS := -I$(ROOT) -I$(ROOT)/include -I$(ROOT)/tests
CXXFLAGS := -std=c++2c -stdlib=libc++ -fno-exceptions -fno-rtti -Wall -Wextra -Werror
LDFLAGS := -L$(ROOT)/.conda/lib -Wl,-rpath,$(ROOT)/.conda/lib
include $(ROOT)/support/toolchain.mk
.DEFAULT_GOAL := all
include $(ROOT)/support/kernel-cxx/flags.mk
CPPFLAGS += -isystem $(KERNEL_CXX_ROOT)/inplace_vector-63569fe8502c3504d7d81bb6d378f3ec21fb1e95/include
KERNEL_CXX_SANITIZERS ?= undefined

LOCAL_TEST_SOURCES := $(sort $(wildcard kernel/*_test.cpp) \
	$(wildcard drivers/*_test.cpp) $(wildcard fs/*_test.cpp) \
	$(wildcard net/*_test.cpp) $(wildcard network/*_test.cpp))
DRIVER_TEST_SOURCES := $(sort \
	$(wildcard ../drivers/fs/fat32/tests/*_test.cpp) \
	$(wildcard ../drivers/fs/ext4/tests/*_test.cpp))
LOCAL_TEST_BINARIES := \
	$(patsubst %.cpp,$(BUILD)/%,$(LOCAL_TEST_SOURCES))
DRIVER_TEST_BINARIES := \
	$(patsubst ../%.cpp,$(BUILD)/%,$(DRIVER_TEST_SOURCES))
BRIDGE_TEST_BINARY := $(BUILD)/qemu/ethgig_tap_self_test
TEST_BINARIES := $(LOCAL_TEST_BINARIES) $(DRIVER_TEST_BINARIES)
TEST_DEPS := $(addsuffix .d,$(TEST_BINARIES))

$(TEST_BINARIES): Makefile
$(TEST_BINARIES): | $(KERNEL_CXX_READY)

.PHONY: all test regression bridge-check fs-driver-check x86-driver-check clean

all: test

test: regression bridge-check fs-driver-check x86-driver-check kernel-cxx-check

.PHONY: kernel-cxx-check
kernel-cxx-check: $(BUILD)/kernel/standard_containers
	$<

$(BUILD)/kernel/standard_containers: kernel/standard_containers.cpp \
    $(ROOT)/kernel/cxx_arena.cpp $(ROOT)/kernel/cxx_runtime.cpp \
    $(ROOT)/kernel/cxx_smoke.cpp $(ROOT)/kernel/cxx_math.cpp $(KERNEL_CXX_READY) \
    $(wildcard $(ROOT)/include/mikos/memory/*.hpp) \
    $(ROOT)/include/mikos/container/standard.hpp Makefile \
    $(ROOT)/support/kernel-cxx/flags.mk
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(KERNEL_CXX_HEADERS) -nostdlib++ \
	  -O2 -g -fsanitize=$(KERNEL_CXX_SANITIZERS) -fno-sanitize-recover=all -fno-omit-frame-pointer \
	  -MMD -MP -MF $@.d $(sort $(filter %.cpp,$^) $(KERNEL_CXX_HASH)) $(LDFLAGS) -o $@

-include $(BUILD)/kernel/standard_containers.d

regression: $(TEST_BINARIES)
	@set -e; for test_binary in $(TEST_BINARIES); do "$$test_binary"; done

bridge-check: $(BRIDGE_TEST_BINARY)
	$< --self-test

x86-driver-check: $(ROOT)/drivers/net/virtio_pci_legacy.cpp \
		$(ROOT)/drivers/net/net.hpp \
		$(ROOT)/drivers/net/virtio.hpp \
		$(ROOT)/drivers/net/virtio_net.hpp \
		$(ROOT)/include/mikos/net/ethernet.hpp
	$(CXX) --target=i386-unknown-elf -m32 -std=c++2c -ffreestanding \
		-fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I$(ROOT) -I$(ROOT)/include -fsyntax-only $<

fs-driver-check: \
		$(ROOT)/drivers/fs/fat32/tests/freestanding_check.cpp \
		$(ROOT)/drivers/fs/ext4/tests/freestanding_check.cpp | $(KERNEL_CXX_READY)
	$(CXX) --target=riscv32-unknown-elf -march=rv32ima_zicsr \
		-mabi=ilp32 -std=c++2c -ffreestanding -fno-exceptions \
		-fno-rtti -Wall -Wextra -Werror $(KERNEL_CXX_TARGET_HEADERS) -I$(ROOT) -I$(ROOT)/include \
		-fsyntax-only $^

$(BUILD)/%: %.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< $(LDFLAGS) -o $@

$(BUILD)/kernel/pseudo_filesystem_test: kernel/pseudo_filesystem_test.cpp \
		$(ROOT)/kernel/pseudo_filesystem.cpp \
		$(ROOT)/kernel/pseudo_filesystem.hpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d \
		kernel/pseudo_filesystem_test.cpp \
		$(ROOT)/kernel/pseudo_filesystem.cpp $(LDFLAGS) -o $@

$(BUILD)/net/stack_test: net/stack_test.cpp $(ROOT)/network/stack.cpp \
		$(ROOT)/include/mikos/net/socket.hpp \
		$(ROOT)/include/mikos/net/tcp.hpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d \
		net/stack_test.cpp $(ROOT)/network/stack.cpp $(LDFLAGS) -o $@

$(BUILD)/drivers/root_device_poll_test: drivers/root_device_poll_test.cpp \
    $(ROOT)/drivers/storage/root_device.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DMIKOS_TRIBE \
		-DMIKOS_TRIBE_INTERACTIVE -DMIKOS_TRIBE_MULTICORE \
		-MMD -MP -MF $@.d drivers/root_device_poll_test.cpp \
		$(ROOT)/drivers/storage/root_device.cpp $(LDFLAGS) -o $@

$(BRIDGE_TEST_BINARY): $(ROOT)/tests/qemu/ethgig_tap.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -Wall -Wextra -Werror $< -o $@

$(BUILD)/drivers/%: ../drivers/%.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< $(LDFLAGS) -o $@

clean:
	rm -f $(TEST_BINARIES) $(TEST_DEPS) $(BUILD)/network/tcp_stack_sanitized $(BUILD)/network/linux_peer

-include $(TEST_DEPS)

.PHONY: network-test network-sanitize
network-test: $(BUILD)/network/tcp_stack_test
	$<

$(BUILD)/network/tcp_stack_test: network/tcp_stack_test.cpp network/harness.cpp \
    network/harness.hpp $(ROOT)/network/stack.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -g -MMD -MP -MF $@.d \
	  network/tcp_stack_test.cpp network/harness.cpp $(ROOT)/network/stack.cpp $(LDFLAGS) -o $@

network-sanitize: $(BUILD)/network/tcp_stack_sanitized
	$<

$(BUILD)/network/tcp_stack_sanitized: network/tcp_stack_test.cpp network/harness.cpp \
    network/harness.hpp $(ROOT)/network/stack.cpp $(wildcard $(ROOT)/include/mikos/net/*.hpp) Makefile | $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined \
	  -fno-sanitize-recover=all -fno-omit-frame-pointer \
	  network/tcp_stack_test.cpp network/harness.cpp $(ROOT)/network/stack.cpp $(LDFLAGS) -o $@

.PHONY: network-linux-peer
network-linux-peer: $(BUILD)/network/linux_peer
	python3 network/linux_peer.py --binary $<

$(BUILD)/network/linux_peer: network/linux_peer.cpp $(ROOT)/network/stack.cpp \
    $(wildcard $(ROOT)/include/mikos/net/*.hpp) Makefile | $(KERNEL_CXX_READY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -g -pthread \
	  network/linux_peer.cpp $(ROOT)/network/stack.cpp $(LDFLAGS) -o $@
