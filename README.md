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

The current proof of concept boots on the supplied RV32 QEMU, enters a
PMP-protected flat U-mode region, and runs bundled static BusyBox and stress-ng
ELFs in sequence. BusyBox prints `MIKOS_BUSYBOX_OK`; stress-ng then runs a
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

## build

Win64 requires the following to be done:
 - Install msys2-x86_64-20240727.exe, Miniconda3-py39_24.7.1-0-Windows-x86_64.exe, run MSYS2 MSYS console
 - git clone https://github.com/mirekez/cpphdl; cd cpphdl

And for Linux:
 - git clone ssh://github.com/mirekez/cpphdl; cd cpphdl
 - wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh; ./Miniconda3-latest-Linux-x86_64.sh
 - source ~/miniconda3/bin/activate; conda init

Then for both Win&Lin:
 - conda create -p ./.conda; source activate base; conda activate ./.conda; conda env update --file requirements.yaml
 - mkdir build; cd build; cmake -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles" ..; make

## author

This software is developed by Mike Reznikov (https://www.linkedin.com/in/mike-reznikov) based on the results of own research.

This work is not subsidized or paid.
