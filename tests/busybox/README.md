# BusyBox compatibility test

This directory owns the user-space workloads used by the current RV32 QEMU
acceptance image. They are tests of MikOS's Linux compatibility surface, not
part of the kernel build implementation.

The build is reproducible from the checked-in BusyBox configuration and pinned
source revisions:

```sh
export RISCV_HOME="$HOME/riscv"
make -C tests/busybox busybox
make -C tests/busybox stress-ng
make -C tests/busybox dropbear
make -C tests/busybox rootfs
```

These targets require an RV32/ILP32 glibc compiler and static libraries.
Run `make userspace-toolchain` from the repository root to build them from
`$RISCV_HOME/riscv-gnu-toolchain` into `$RISCV_HOME/linux`; see the
[setup instructions](../../README.md#build-and-test). The build automatically
detects this installation as well as `$RISCV_HOME/bin/riscv32-unknown-linux-gnu-gcc`.
Set `RISCV_LINUX_PREFIX` for a different prefix. The preflight statically links
a small program to catch missing libraries or an incompatible target ABI.
The bare-metal `riscv32-unknown-elf-*` tools suffice for `make kernel`.
BusyBox and Dropbear run on mikOS using its Linux-compatible syscall ABI;
no Linux guest kernel is needed.

The `tc` applet is disabled: mikOS does not implement its traffic-control
interface, and the pinned applet depends on CBQ definitions removed from newer
Linux headers.

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

Dropbear 2026.94 is checksum-pinned, built at the `0x81400000` flat-user
load address (BusyBox and stress-ng use `0x81000000`), and configured for public-key
root login without passwords or forwarding. BusyBox init reads `/etc/inittab`,
runs `/etc/init.d/rcS`, configures `eth0` as `192.168.76.2/24`, and starts
Dropbear on port 22 with persistent on-demand host-key generation. The image's
test-only authorized key can be replaced at
`rootfs/root/.ssh/authorized_keys` before rebuilding.

The build also generates a local test client identity at `build/mikos_ssh_key`
when absent and adds its public key to the staged image's `authorized_keys`.
An existing private key is preserved. `make dropbear-client` converts that
identity to the Dropbear format used by the interactive launcher; private keys
stay in the ignored build directory.

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

The pinned stress-ng source snapshot and MikOS-only patch live under
`third_party/` and `patches/` here. The build copies the vendored source into
the build directory before applying the patch; no submodule setup is needed.
