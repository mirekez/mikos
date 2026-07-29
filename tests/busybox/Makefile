SHELL := /bin/bash

ROOT := $(abspath ../..)
BUILD ?= $(ROOT)/build/tests/busybox
RISCV_PREFIX ?= /home/me/riscv/bin/riscv32-unknown-linux-gnu-
LD := $(RISCV_PREFIX)ld
LLVM_READELF := $(ROOT)/.conda/bin/llvm-readelf
JOBS ?= 4

BUSYBOX_COMMIT := ead17e7808236be80614d0a8755f4ddad63ab56c
BUSYBOX_REPOSITORY ?= https://git.busybox.net/busybox
BUSYBOX_REFERENCE ?=
BUSYBOX_SOURCE := $(BUILD)/busybox-source
BUSYBOX_OUT := $(BUILD)/busybox
BUSYBOX := $(BUSYBOX_OUT)/busybox
BUSYBOX_CONFIG := config/busybox.config
BUSYBOX_ADDRESS := 0x81000000
BUSYBOX_BLOB := $(BUILD)/busybox_blob.o

STRESS_NG_REFERENCE := third_party/stress-ng
STRESS_NG_COMMIT := f73c6bd5f34df31dbaea8215e1e9c3f5cbc8d060
STRESS_NG_SOURCE := $(BUILD)/stress-ng-source
STRESS_NG := $(STRESS_NG_SOURCE)/stress-ng
STRESS_NG_ADDRESS := 0x81000000
STRESS_NG_PATCH := patches/stress-ng-mikos.patch
STRESS_NG_BLOB := $(BUILD)/stress_ng_blob.o

.PHONY: all payloads busybox stress-ng clean

all: payloads

payloads: $(BUSYBOX_BLOB) $(STRESS_NG_BLOB)

busybox: $(BUSYBOX)

stress-ng: $(STRESS_NG)

$(BUSYBOX_SOURCE)/.source-ready: download_busybox.sh
	@mkdir -p $(BUILD)
	BUSYBOX_REFERENCE='$(BUSYBOX_REFERENCE)' \
	BUSYBOX_REPOSITORY='$(BUSYBOX_REPOSITORY)' \
		./download_busybox.sh $(BUSYBOX_SOURCE) $(BUSYBOX_COMMIT)

$(BUSYBOX_OUT)/.config: $(BUSYBOX_SOURCE)/.source-ready $(BUSYBOX_CONFIG)
	@mkdir -p $(BUSYBOX_OUT)
	cp $(BUSYBOX_CONFIG) $@

$(BUSYBOX): $(BUSYBOX_OUT)/.config build_busybox.sh
	./build_busybox.sh $(BUSYBOX_SOURCE) $(BUSYBOX_OUT) \
		$(RISCV_PREFIX) $(BUSYBOX_ADDRESS) $(LLVM_READELF) $(JOBS)

$(BUSYBOX_BLOB): $(BUSYBOX)
	cd $(BUILD) && $(LD) -m elf32lriscv -r -b binary \
		busybox/busybox -o busybox_blob.o

$(STRESS_NG_SOURCE)/.source-ready: $(STRESS_NG_PATCH)
	rm -rf $(STRESS_NG_SOURCE)
	@mkdir -p $(STRESS_NG_SOURCE)
	git -C $(STRESS_NG_REFERENCE) archive --format=tar $(STRESS_NG_COMMIT) | \
		tar -xf - -C $(STRESS_NG_SOURCE)
	patch -d $(STRESS_NG_SOURCE) -p1 < $(STRESS_NG_PATCH)
	@touch $@

$(STRESS_NG): $(STRESS_NG_SOURCE)/.source-ready
	$(MAKE) -C $(STRESS_NG_SOURCE) -j$(JOBS) \
		CC=$(RISCV_PREFIX)gcc CXX=$(RISCV_PREFIX)g++ \
		STATIC=1 BUILD_SMALL=1 GARBAGE_COLLECT=1 \
		CFLAGS=-DSTRESS_MIKOS \
		LDFLAGS='-Wl,-Ttext-segment=$(STRESS_NG_ADDRESS)' \
		LIB_ACL= LIB_AIO= LIB_APPARMOR= LIB_BSD= LIB_CAP= \
		LIB_CRYPT= LIB_EGL= LIB_GBM= LIB_GLES2= LIB_IPSEC_MB= \
		LIB_JPEG= LIB_JUDY= LIB_KMOD= LIB_LZMA= LIB_MD= \
		LIB_MPFR= LIB_NL= LIB_SCTP= LIB_XXHASH= LIB_Z= stress-ng
	$(RISCV_PREFIX)strip $@
	$(LLVM_READELF) -l $@ > $(STRESS_NG_SOURCE)/program-headers.txt
	rg -q 'LOAD +0x[0-9a-f]+ +$(STRESS_NG_ADDRESS)' \
		$(STRESS_NG_SOURCE)/program-headers.txt

$(STRESS_NG_BLOB): $(STRESS_NG)
	cd $(BUILD) && $(LD) -m elf32lriscv -r -b binary \
		stress-ng-source/stress-ng -o stress_ng_blob.o

clean:
	rm -rf $(BUILD)
