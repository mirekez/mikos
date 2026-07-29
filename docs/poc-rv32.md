# RV32 BusyBox POC

The first proof of concept intentionally has:

- one process that runs static BusyBox and stress-ng images in sequence;
- no scheduler or context switch;
- no filesystem, `fork`, or `exec`;
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

After BusyBox exits, the kernel replaces its syscall trap frame and reloads the
stress-ng ELF into the same flat user region. Details and limitations are in
[stress-ng.md](stress-ng.md).
