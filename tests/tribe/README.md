# Tribe acceptance test

`make tribe-test` clones the pinned cpphdl revision, builds the 64-bit-bus Tribe
simulator with 32 MiB RAM, builds the MikOS Tribe image, and exercises:

- the polling NS16550A UART;
- a block-zero read through the `tribe_sd` controller;
- BusyBox startup and `echo` under MikOS;
- rootless Ethernet ARP plus an IPv4 ICMP echo request/reply.

The default clone can be replaced with a local cpphdl checkout:

```sh
CPPHDL_REFERENCE=/path/to/cpphdl make tribe-test
```

`CPPHDL_REVISION`, `CPPHDL_REPOSITORY`, `CPPHDL_TOOLCHAIN`, `RISCV_HOME`,
`JOBS`, `TRIBE_CYCLES`, and `TRIBE_TIMEOUT` are also configurable.

For an interactive UART session, run:

```sh
tests/tribe/tribe_interactive.sh
```

This builds a separate Tribe image that starts `busybox sh -i`. Simulator stdin
is connected to the NS16550A RX path and guest UART TX is printed directly to
the terminal. The interactive profile implements BusyBox's constrained
`vfork`/`execve`/`wait4` applet lifecycle and exposes a small synthetic `/` and
`/proc`, so commands including `ls` and `top` work. Type `q` to leave `top`,
`exit` to shut down cleanly, Ctrl+C to stop the simulator, or Ctrl+Z to suspend
it. `TRIBE_INTERACTIVE_CYCLES` controls the safety limit.

The preparation script removes `ENABLE_RV32IA`, `ENABLE_ISR`, and
`ENABLE_MMU_TLB` from the test clone's `tribe/Config.h`. MikOS handles libc's
occasional RV32A word operations in its illegal-instruction trap and uses only
polling drivers on this board. `ENABLE_ZICSR` and `ENABLE_TRAPS` remain enabled
because entering user mode and servicing BusyBox `ecall` instructions require
them; disabling either would remove the mechanism that implements system calls.
The minimal Tribe CSR model also excludes PMP, so this test emits
`MIKOS:PMP_UNAVAILABLE`; the normal MikOS/QEMU build still programs PMP.
