# Shared by kernel builds and the isolated host/target container checks.
KERNEL_CXX_ROOT := $(ROOT)/build/kernel-cxx
KERNEL_CXX_READY := $(KERNEL_CXX_ROOT)/.ready
KERNEL_CXX_HEADERS := -nostdinc++ \
  -isystem $(KERNEL_CXX_ROOT)/include \
  -isystem $(KERNEL_CXX_ROOT)/libcxx-21.1.3.src/include \
  -isystem $(KERNEL_CXX_ROOT)/inplace_vector-63569fe8502c3504d7d81bb6d378f3ec21fb1e95/include
KERNEL_CXX_HASH := $(KERNEL_CXX_ROOT)/libcxx-21.1.3.src/src/hash.cpp
KERNEL_CXX_BUILTINS := $(KERNEL_CXX_ROOT)/compiler-rt-21.1.3.src/lib/builtins
# Only target C declarations are used. No Linux libc or libstdc++ is linked.
RISCV_SYSROOT ?= $(shell $(RISCV_PREFIX)gcc -print-sysroot)
KERNEL_CXX_TARGET_HEADERS := $(KERNEL_CXX_HEADERS) --sysroot=$(RISCV_SYSROOT) \
  -isystem $(RISCV_SYSROOT)/usr/include

$(KERNEL_CXX_READY): $(ROOT)/support/kernel-cxx/prepare.sh \
                     $(ROOT)/support/kernel-cxx/configure.cmake \
                     $(ROOT)/support/kernel-cxx/assertion_handler.hpp
	 bash $(ROOT)/support/kernel-cxx/prepare.sh
