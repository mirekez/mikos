# Tribe acceptance test

`make tribe-test` clones the pinned cpphdl revision, builds the 64-bit-bus Tribe
simulator with 32 MiB RAM, builds the MikOS Tribe image, and exercises:

- the polling NS16550A UART;
- mounting the shared ext4 root through the `tribe_sd` controller;
- BusyBox startup and `echo` under MikOS;
- rootless Ethernet ARP plus an IPv4 ICMP echo request/reply.

The SD driver uses DMA for aligned multi-sector payload reads. Reused
single-sector cache fills, and multicore reads into reusable kernel metadata
buffers, use the PIO RX register so the CPU cannot observe stale data while
Tribe's external D-cache invalidation retires. The runner
attaches `build/tests/busybox/rootfs.ext4`, and boot must find both executables
and report `MIKOS:EXT4_ROOT_OK`. The host regression suite also checks DMA
register sequencing, completion, controller errors, timeouts, and destination
validation.

The default clone can be replaced with a local cpphdl checkout:

```sh
CPPHDL_REFERENCE=/path/to/cpphdl make tribe-test
```

`CPPHDL_REVISION`, `CPPHDL_REPOSITORY`, `CPPHDL_TOOLCHAIN`, `RISCV_HOME`,
`JOBS`, `TRIBE_CYCLES`, and `TRIBE_TIMEOUT` are also configurable.

For an interactive UART session, run:

```sh
tests/tribe/tribe_interactive.sh --multicore \
  --tap-socket /tmp/tribe-ethgig.sock
```

The TAP bridge socket defaults to `/tmp/tribe-ethgig.sock` and can also be
selected with `TRIBE_ETH_TAP_SOCKET`. With `tap-tribe` configured as
`192.168.76.1/24`, configure the guest at the BusyBox prompt and ping it from
the host:

```sh
# MikOS BusyBox shell
ifconfig eth0 192.168.76.2 netmask 255.255.255.0 up

# Host shell
ping -I tap-tribe -c 4 192.168.76.2
```

The root-dependent end-to-end regression uses an already running TAP bridge
and requires four consecutive echo replies, guarding against simulator-side
rate limiting or receive rearm regressions:

```sh
make tribe-interactive-ping-test
```

Pass `--multicore` to `prepare_cpphdl.sh`, `run_tribe.sh`, or
`tribe_interactive.sh` to build or run cpphdl's four-core Tribe simulator. In
interactive `top`, press `1` to display the separate `CPU0` through `CPU3`
lines.

This builds a separate Tribe image that starts `busybox sh -i`. Simulator stdin
is connected to the NS16550A RX path and guest UART TX is printed directly to
the terminal. The interactive profile implements BusyBox's bounded serial
`fork`/`vfork`/`execve`/`waitid` applet lifecycle. The root directory, regular files,
directories, metadata, executable lookup, and supported mutations come from the attached
ext4 image; `/proc` remains a virtual overlay. Relative paths are resolved
against the current directory, so an executable in `/bin` can be launched in
the usual way:

```sh
cd /bin
./stress-ng --cpu 1 --cpu-method loop --cpu-ops 4 --verify --metrics-brief
```

`execve` can load any executable static RV32 ELF in the image whose segments
fit the flat user address range; it is not special-cased to stress-ng.

With the existing `tap-tribe` bridge, `make tribe-interactive-tcp-test` starts
BusyBox `nc -l` in this profile and transfers a marker from the host through a
real TCP handshake and accepted stream. This validates the passive socket
slice; it does not imply that Dropbear's fork/pipe/PTY process model is ready.

`make tribe-interactive-process-test` exercises ordinary glibc/BusyBox fork,
pipe inheritance, fd-10 redirection preservation, three-stage pipelines,
parent/child address-image restoration, and nonzero child-status reaping on
the multicore Tribe profile. It does not claim that parent and child are both
runnable; background ordering and blocking-pipe scheduling remain pending.

Type `q` to leave `top`, `exit` to shut down cleanly, Ctrl+C to stop the
simulator, or Ctrl+Z to suspend it. `TRIBE_INTERACTIVE_CYCLES` controls the
safety limit.

For the default single-core build, the preparation script removes
`ENABLE_RV32IA`, `ENABLE_ISR`, and `ENABLE_MMU_TLB` from the test clone's
`tribe/Config.h`. The multicore build retains them because cpphdl's multicore
target requires its atomic and cross-hart fence interfaces. MikOS uses polling
drivers on this board. `ENABLE_ZICSR` and `ENABLE_TRAPS` remain enabled because
entering user mode and servicing BusyBox `ecall` instructions require them;
disabling either would remove the mechanism that implements system calls. The
minimal single-core Tribe CSR model also excludes PMP, so that test emits
`MIKOS:PMP_UNAVAILABLE`; the normal MikOS/QEMU build still programs PMP.
