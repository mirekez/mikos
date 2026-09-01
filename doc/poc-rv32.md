# RV32 BusyBox POC

The current proof of concept has:

- one materialized flat user image plus one bounded suspended-parent snapshot;
- no runnable multi-process scheduler or arbitrary process context switch;
- a writable ext4 boot volume for the gated no-journal/no-metadata-checksum
  format;
- ordinary glibc fork-shaped `clone`, vfork-shaped `clone`, `execve`, `pipe2`,
  group-aware wait/reap, signal masks/actions, sessions/groups, and bounded PTY
  mechanisms; the live adapter still serializes parent and child execution;
- one flat physical user region from `0x81000000` to `0x82000000`;
- M-mode kernel and U-mode application;
- PMP denying U-mode access to the kernel and devices;
- synchronous `ecall` dispatch;
- one CLINT machine-timer interrupt used only for U-mode preemption;
- polling-only QEMU 16550 UART;
- only the syscalls observed or strongly required for the BusyBox echo and
  bounded stress-ng CPU acceptance workloads.

The supplied BusyBox is linked at `0x10000`, outside QEMU `virt` RAM. The build
therefore rebuilds the same source out-of-tree at `0x81000000`; it does not
modify the supplied source checkout.

This QEMU source tree has a non-upstream `virt` device layout: its generated
device tree places the 16550 UART at `0x82000000`. The user region ends at that
address so user code cannot access the device and the POC needs no memory hole.

Build and run:

```sh
export PATH="$PWD/.conda/bin:$PATH"
make test
make busybox
make stress-ng
make -C tests/busybox rootfs
make kernel
make qemu-test
```

QEMU 10.1 is configured from
`/home/me/riscv/riscv-gnu-toolchain/qemu` into `build/qemu`. To rebuild it:

```sh
mkdir -p build/qemu
cd build/qemu
/home/me/riscv/riscv-gnu-toolchain/qemu/configure \
  --target-list=riscv32-softmmu --disable-docs --disable-gtk \
  --disable-sdl --disable-werror --disable-slirp
ninja qemu-system-riscv32
```

The kernel itself is compiled by the Conda Clang C++ compiler in C++26 mode.
The existing RISC-V GNU linker is used because this Conda environment does not
contain LLD.

## Forced-preemption proof

Before BusyBox, the kernel starts a relocatable two-instruction U-mode loop that
contains no syscall or cooperative yield. The 20 ms M-mode timer interrupt
preempts that loop. The architecture-neutral scheduler replaces the loop's
saved trap frame with the prepared BusyBox entry frame, and the existing trap
return enters BusyBox. No extra context-switch assembly is needed.

The QEMU test requires a nonzero preemption count and zero timer-contract
violations. The timer handler only rearms the timer and changes bounded state;
it performs no device work, allocation, formatting, logging, or blocking.

After BusyBox exits, the kernel replaces its syscall trap frame and loads the
stress-ng ELF from `/bin/stress-ng` in the attached ext4 image into the same
flat user region. Details and limitations are in [stress-ng.md](stress-ng.md).

The interactive Tribe profile additionally snapshots the parent's writable,
anonymous, and stack regions before a child runs, retains inherited descriptor
resources, reloads the parent's BusyBox text, and restores its saved regions on
child exit. This is enough for bounded shell pipelines and proves parent/child
data and fd-10 preservation. It is not concurrent fork: a child that blocks on
a full pipe cannot yet yield to its suspended parent, and nested/background
process ordering awaits the runnable process table described in
[process-ipc-tty-plan.md](process-ipc-tty-plan.md).
