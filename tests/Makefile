SHELL := /bin/bash

ROOT := $(abspath ..)
BUILD := $(ROOT)/build/tests/regression
CXX := $(ROOT)/.conda/bin/clang++
CPPFLAGS := -I$(ROOT)/include -I$(ROOT)/tests
CXXFLAGS := -std=c++2c -fno-exceptions -fno-rtti -Wall -Wextra -Werror

TEST_SOURCES := $(sort $(wildcard kernel/*_test.cpp) \
	$(wildcard drivers/*_test.cpp) $(wildcard net/*_test.cpp))
TEST_BINARIES := $(patsubst %.cpp,$(BUILD)/%,$(TEST_SOURCES))
TEST_DEPS := $(addsuffix .d,$(TEST_BINARIES))

.PHONY: all test regression x86-driver-check clean

all: test

test: regression x86-driver-check

regression: $(TEST_BINARIES)
	@set -e; for test_binary in $(TEST_BINARIES); do "$$test_binary"; done

x86-driver-check: $(ROOT)/drivers/net/virtio_pci_legacy.cpp \
		$(ROOT)/include/mikos/drivers/net.hpp \
		$(ROOT)/include/mikos/drivers/virtio.hpp \
		$(ROOT)/include/mikos/drivers/virtio_net.hpp \
		$(ROOT)/include/mikos/net/ethernet.hpp
	$(CXX) --target=i386-unknown-elf -m32 -std=c++2c -ffreestanding \
		-fno-exceptions -fno-rtti -Wall -Wextra -Werror \
		-I$(ROOT)/include -fsyntax-only $<

$(BUILD)/%: %.cpp support/test.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< -o $@

clean:
	rm -f $(TEST_BINARIES) $(TEST_DEPS)

-include $(TEST_DEPS)
