SHELL := /bin/bash

ROOT := $(abspath ..)
BUILD := $(ROOT)/build/tests/regression
CXX := $(ROOT)/.conda/bin/clang++
CPPFLAGS := -I$(ROOT) -I$(ROOT)/include -I$(ROOT)/tests
CXXFLAGS := -std=c++2c -fno-exceptions -fno-rtti -Wall -Wextra -Werror

LOCAL_TEST_SOURCES := $(sort $(wildcard kernel/*_test.cpp) \
	$(wildcard drivers/*_test.cpp) $(wildcard fs/*_test.cpp) \
	$(wildcard net/*_test.cpp))
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

.PHONY: all test regression bridge-check fs-driver-check x86-driver-check clean

all: test

test: regression bridge-check fs-driver-check x86-driver-check

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
		$(ROOT)/drivers/fs/ext4/tests/freestanding_check.cpp
	$(CXX) --target=riscv32-unknown-elf -march=rv32ima_zicsr \
		-mabi=ilp32 -std=c++2c -ffreestanding -fno-exceptions \
		-fno-rtti -Wall -Wextra -Werror -I$(ROOT) -I$(ROOT)/include \
		-fsyntax-only $^

$(BUILD)/%: %.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< -o $@

$(BUILD)/kernel/pseudo_filesystem_test: kernel/pseudo_filesystem_test.cpp \
		$(ROOT)/kernel/pseudo_filesystem.cpp \
		$(ROOT)/kernel/pseudo_filesystem.hpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d \
		kernel/pseudo_filesystem_test.cpp \
		$(ROOT)/kernel/pseudo_filesystem.cpp -o $@

$(BUILD)/net/stack_test: net/stack_test.cpp $(ROOT)/network/stack.cpp \
		$(ROOT)/include/mikos/net/socket.hpp \
		$(ROOT)/include/mikos/net/tcp.hpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d \
		net/stack_test.cpp $(ROOT)/network/stack.cpp -o $@

$(BRIDGE_TEST_BINARY): $(ROOT)/tests/qemu/ethgig_tap.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++2c -Wall -Wextra -Werror $< -o $@

$(BUILD)/drivers/%: ../drivers/%.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< -o $@

clean:
	rm -f $(TEST_BINARIES) $(TEST_DEPS)

-include $(TEST_DEPS)
