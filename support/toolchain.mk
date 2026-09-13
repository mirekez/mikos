# Clang compiles the kernel; either Newlib or Linux supplies target C headers
# and GNU binutils. Linux userspace workloads require the Linux toolchain.
RISCV_HOME ?= $(HOME)/riscv
RISCV_PREFIX ?= $(if $(wildcard $(RISCV_HOME)/bin/riscv32-unknown-linux-gnu-gcc),$(RISCV_HOME)/bin/riscv32-unknown-linux-gnu-,$(RISCV_HOME)/bin/riscv32-unknown-elf-)
RISCV_LINUX_PREFIX ?= $(if $(findstring linux,$(RISCV_PREFIX)),$(RISCV_PREFIX),$(RISCV_HOME)/bin/riscv32-unknown-linux-gnu-)
export RISCV_HOME

# Keep discovery lazy: native regressions and clean do not need a cross compiler.
RISCV_SYSROOT ?= $(shell $(RISCV_PREFIX)gcc -print-sysroot 2>/dev/null)
RISCV_C_INCLUDE = $(if $(strip $(RISCV_SYSROOT)),$(firstword $(wildcard $(RISCV_SYSROOT)/usr/include $(RISCV_SYSROOT)/include)))
