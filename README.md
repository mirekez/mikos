# MikOS

Lightweight C++ replacement for Linux without MMU/TLB, exceptions and interrupts.

Mikos is an experimental, test-driven operating system with a minimal
mechanism-only kernel, flat-memory architecture profiles, polling instead of
ordinary device interrupts, and generated Linux syscall ABI tables.

Start with:

- [Requirements](REQUIREMENTS.md)
- [Development plan](PLAN.md)
- [RV32 BusyBox proof of concept](docs/poc-rv32.md)
- [Polling network proof of concept](docs/network-poc.md)
- [Regression and acceptance tests](tests/README.md)

The current proof of concept boots on the supplied RV32 QEMU, enters a
PMP-protected flat U-mode region, mounts an ext4 root image, and loads static
BusyBox and stress-ng ELFs from `/bin` in sequence. BusyBox prints
`MIKOS_BUSYBOX_OK`; stress-ng then runs a
verified four-operation `cpu/loop` workload and reports its own metrics and
success. A sole CLINT scheduling timer also demonstrably preempts an
unmodified, non-cooperative U-mode loop before starting BusyBox; the PLIC and
all device interrupts remain disabled. See
[the stress-ng integration](docs/stress-ng.md) for the exact supported scope.

The first networking slice discovers QEMU's modern virtio-net device over
virtio-mmio, uses polling-only split queues, assigns the fixed POC address
`10.0.2.15`, and passes an end-to-end ARP reply test. This transport is kept
behind a small shared NIC interface so the x86 PCI transport can reuse the
Ethernet/IP code. SSH and `top` are not supported yet; their explicit gates are
listed in the network POC document.

## Build and test

Create the local Conda environment, then use the root Makefile for kernel and
regression entry points:

```sh
git submodule update --init
conda create -p ./.conda
conda env update -p ./.conda --file requirements.yaml
make test
make kernel
```

The kernel target requests the pinned BusyBox and stress-ng acceptance
workloads from `tests/busybox/` and packages them as
`build/tests/busybox/rootfs.ext4`; they are not linked into the kernel. The
first build downloads BusyBox; an existing checkout can be selected with
`BUSYBOX_REFERENCE=/path/to/busybox`. QEMU acceptance runners are under
`tests/qemu/` and remain available through `make qemu-test` and
`make qemu-net-test`.

`make tribe-test` builds a pinned cpphdl Tribe simulator and attaches the same
ext4 root image through the Tribe SD controller. Its rootless network peer
verifies ARP and IPv4 ICMP echo replies; see `tests/tribe/README.md`.

## author

This software is developed by Mike Reznikov (https://www.linkedin.com/in/mike-reznikov) based on the results of own research.

This work is not subsidized or paid.
