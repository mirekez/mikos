# BusyBox compatibility test

This directory owns the user-space workloads used by the current RV32 QEMU
acceptance image. They are tests of MikOS's Linux compatibility surface, not
part of the kernel build implementation.

The build is reproducible from the checked-in BusyBox configuration and pinned
source revisions:

```sh
make -C tests/busybox busybox
make -C tests/busybox stress-ng
make -C tests/busybox dropbear
make -C tests/busybox rootfs
```

`download_busybox.sh` fetches the pinned BusyBox revision into
`build/tests/busybox`. To use an existing checkout without network access:

```sh
make -C tests/busybox \
  BUSYBOX_REFERENCE=/path/to/busybox \
  rootfs
```

`build_busybox.sh` performs the out-of-tree cross-build and verifies the ELF
load address. The rootfs rule installs `/bin/busybox`, its required `sh`,
`mkdir`, `netstat`, and `init` links, `/bin/stress-ng`, and a statically linked
RV32 Dropbear server into a generated 16 MiB ext4 image with a fixed UUID and
without a journal or metadata checksums. Both QEMU virtio-blk and Tribe SD
attach that exact image.

Dropbear 2026.94 is checksum-pinned, built at the same `0x81000000` flat-user
load address as the other sequential workloads, and configured for public-key
root login without passwords or forwarding. BusyBox init reads `/etc/inittab`,
runs `/etc/init.d/rcS`, configures `eth0` as `192.168.76.2/24`, and starts
Dropbear on port 22 with persistent on-demand host-key generation. The image's
test-only authorized key can be replaced at
`rootfs/root/.ssh/authorized_keys` before rebuilding.

An existing Dropbear source tree or release archive can be used without a
download:

```sh
make -C tests/busybox \
  DROPBEAR_REFERENCE=/path/to/dropbear-2026.94.tar.bz2 rootfs
```

This supplies the RV32 server and its boot service. MikOS's current network
POC still implements ARP/ICMP and UDP control descriptors only; serving an SSH
connection under MikOS additionally requires the planned TCP stream socket,
`bind`/`listen`/`accept`, descriptor I/O, polling, and concurrent-process
slice. A Linux RV32 runtime invoking `/sbin/init` can start this image's server
now.

The pinned stress-ng Git submodule and MikOS-only patch live under
`third_party/` and `patches/` here so the complete compatibility test is
self-contained. Initialize the submodule with `git submodule update --init`
after cloning MikOS.
