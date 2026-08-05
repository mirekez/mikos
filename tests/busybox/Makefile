SHELL := /bin/bash

ROOT := $(abspath ../..)
BUILD ?= $(ROOT)/build/tests/busybox
RISCV_PREFIX ?= /home/me/riscv/bin/riscv32-unknown-linux-gnu-
LD := $(RISCV_PREFIX)ld
LLVM_READELF := $(ROOT)/.conda/bin/llvm-readelf
JOBS ?= 4
MKE2FS ?= /usr/sbin/mke2fs
E2FSCK ?= /usr/sbin/e2fsck
DEBUGFS ?= /usr/sbin/debugfs

BUSYBOX_COMMIT := ead17e7808236be80614d0a8755f4ddad63ab56c
BUSYBOX_REPOSITORY ?= https://git.busybox.net/busybox
BUSYBOX_REFERENCE ?=
BUSYBOX_SOURCE := $(BUILD)/busybox-source
BUSYBOX_OUT := $(BUILD)/busybox
BUSYBOX := $(BUSYBOX_OUT)/busybox
BUSYBOX_CONFIG := config/busybox.config
BUSYBOX_ADDRESS := 0x81000000

STRESS_NG_REFERENCE := third_party/stress-ng
STRESS_NG_COMMIT := f73c6bd5f34df31dbaea8215e1e9c3f5cbc8d060
STRESS_NG_SOURCE := $(BUILD)/stress-ng-source
STRESS_NG := $(STRESS_NG_SOURCE)/stress-ng
STRESS_NG_ADDRESS := 0x81000000
STRESS_NG_PATCH := patches/stress-ng-mikos.patch

DROPBEAR_VERSION := 2026.94
DROPBEAR_SHA256 := e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
DROPBEAR_REPOSITORY ?= https://matt.ucc.asn.au/dropbear/releases
DROPBEAR_REFERENCE ?=
DROPBEAR_SOURCE := $(BUILD)/dropbear-source
DROPBEAR := $(DROPBEAR_SOURCE)/dropbear
DROPBEAR_CONFIG := config/dropbear_localoptions.h
DROPBEAR_PATCH := patches/dropbear-mikos.patch
DROPBEAR_ADDRESS := 0x81000000

ROOTFS_STAGE := $(BUILD)/rootfs
ROOTFS_IMAGE := $(BUILD)/rootfs.ext4
ROOTFS_SIZE ?= 16M
ROOTFS_CONFIG := rootfs/etc/group rootfs/etc/inittab rootfs/etc/passwd \
	rootfs/etc/shells rootfs/etc/init.d/rcS \
	rootfs/etc/dropbear/dropbear_ed25519_host_key.b64 \
	rootfs/root/.ssh/authorized_keys

.PHONY: all payloads rootfs busybox stress-ng dropbear clean

all: rootfs

payloads: rootfs

rootfs: $(ROOTFS_IMAGE)

busybox: $(BUSYBOX)

stress-ng: $(STRESS_NG)

dropbear: $(DROPBEAR)

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

$(DROPBEAR_SOURCE)/.source-ready: download_dropbear.sh
	@mkdir -p $(BUILD)
	DROPBEAR_REFERENCE='$(DROPBEAR_REFERENCE)' \
	DROPBEAR_REPOSITORY='$(DROPBEAR_REPOSITORY)' \
		./download_dropbear.sh $(DROPBEAR_SOURCE) $(DROPBEAR_VERSION) \
		$(DROPBEAR_SHA256)

$(DROPBEAR): $(DROPBEAR_SOURCE)/.source-ready $(DROPBEAR_CONFIG) \
		$(DROPBEAR_PATCH) \
		build_dropbear.sh
	./build_dropbear.sh $(DROPBEAR_SOURCE) $(RISCV_PREFIX) \
		$(DROPBEAR_ADDRESS) $(LLVM_READELF) $(JOBS) $(DROPBEAR_CONFIG) \
		$(DROPBEAR_PATCH)

$(ROOTFS_IMAGE): Makefile $(BUSYBOX) $(STRESS_NG) $(DROPBEAR) \
		$(ROOTFS_CONFIG) set_rootfs_owners.sh verify_rootfs.sh
	rm -rf $(ROOTFS_STAGE)
	install -d $(ROOTFS_STAGE)/bin $(ROOTFS_STAGE)/sbin \
		$(ROOTFS_STAGE)/usr/sbin \
		$(ROOTFS_STAGE)/etc/init.d $(ROOTFS_STAGE)/etc/dropbear \
		$(ROOTFS_STAGE)/root/.ssh $(ROOTFS_STAGE)/run \
		$(ROOTFS_STAGE)/var/run $(ROOTFS_STAGE)/dev/pts \
		$(ROOTFS_STAGE)/proc $(ROOTFS_STAGE)/sys
	install -m 0755 $(BUSYBOX) $(ROOTFS_STAGE)/bin/busybox
	ln $(ROOTFS_STAGE)/bin/busybox $(ROOTFS_STAGE)/bin/mkdir
	ln $(ROOTFS_STAGE)/bin/busybox $(ROOTFS_STAGE)/bin/netstat
	ln $(ROOTFS_STAGE)/bin/busybox $(ROOTFS_STAGE)/bin/sh
	ln $(ROOTFS_STAGE)/bin/busybox $(ROOTFS_STAGE)/sbin/init
	install -m 0755 $(STRESS_NG) $(ROOTFS_STAGE)/bin/stress-ng
	install -m 0755 $(DROPBEAR) $(ROOTFS_STAGE)/usr/sbin/dropbear
	install -m 0644 rootfs/etc/group $(ROOTFS_STAGE)/etc/group
	install -m 0644 rootfs/etc/inittab $(ROOTFS_STAGE)/etc/inittab
	install -m 0644 rootfs/etc/passwd $(ROOTFS_STAGE)/etc/passwd
	install -m 0644 rootfs/etc/shells $(ROOTFS_STAGE)/etc/shells
	install -m 0755 rootfs/etc/init.d/rcS $(ROOTFS_STAGE)/etc/init.d/rcS
	base64 -d rootfs/etc/dropbear/dropbear_ed25519_host_key.b64 \
		>$(ROOTFS_STAGE)/etc/dropbear/dropbear_ed25519_host_key
	chmod 0600 $(ROOTFS_STAGE)/etc/dropbear/dropbear_ed25519_host_key
	install -m 0600 rootfs/root/.ssh/authorized_keys \
		$(ROOTFS_STAGE)/root/.ssh/authorized_keys
	touch -d @946684800 $(ROOTFS_STAGE) $(ROOTFS_STAGE)/bin \
		$(ROOTFS_STAGE)/sbin $(ROOTFS_STAGE)/usr \
		$(ROOTFS_STAGE)/usr/sbin \
		$(ROOTFS_STAGE)/etc $(ROOTFS_STAGE)/etc/init.d \
		$(ROOTFS_STAGE)/etc/dropbear $(ROOTFS_STAGE)/root \
		$(ROOTFS_STAGE)/root/.ssh $(ROOTFS_STAGE)/run \
		$(ROOTFS_STAGE)/var $(ROOTFS_STAGE)/var/run \
		$(ROOTFS_STAGE)/dev $(ROOTFS_STAGE)/dev/pts \
		$(ROOTFS_STAGE)/proc $(ROOTFS_STAGE)/sys \
		$(ROOTFS_STAGE)/bin/busybox $(ROOTFS_STAGE)/bin/mkdir \
		$(ROOTFS_STAGE)/bin/netstat \
		$(ROOTFS_STAGE)/bin/sh $(ROOTFS_STAGE)/sbin/init \
		$(ROOTFS_STAGE)/bin/stress-ng \
		$(ROOTFS_STAGE)/usr/sbin/dropbear \
		$(ROOTFS_STAGE)/etc/dropbear/dropbear_ed25519_host_key \
		$(ROOTFS_STAGE)/etc/group $(ROOTFS_STAGE)/etc/inittab \
		$(ROOTFS_STAGE)/etc/passwd $(ROOTFS_STAGE)/etc/shells \
		$(ROOTFS_STAGE)/etc/init.d/rcS \
		$(ROOTFS_STAGE)/root/.ssh/authorized_keys
	truncate -s $(ROOTFS_SIZE) $@
	E2FSPROGS_FAKE_TIME=946684800 \
	$(MKE2FS) -q -F -t ext4 -b 1024 -I 128 -m 0 \
		-U 4d494b4f-5352-4f4f-5400-000000000001 \
		-O extent,filetype,^64bit,^metadata_csum,^has_journal \
		-E lazy_itable_init=0 -d $(ROOTFS_STAGE) $@
	./set_rootfs_owners.sh $(ROOTFS_STAGE) $@ $(DEBUGFS)
	$(E2FSCK) -fn $@
	./verify_rootfs.sh $@ $(DEBUGFS)
	@printf 'rootfs.ext4: '
	@du -h $@ | cut -f1

clean:
	rm -rf $(BUILD)
