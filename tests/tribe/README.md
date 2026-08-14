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

`make tribe-interactive-ssh-test` configures the guest address, starts
Dropbear with `-F` as a BusyBox background job, waits for the kernel TCP listen
marker, and then verifies a real host SSH connection through `accept`,
identification, key exchange, none-mode test authentication, nested remote
command execution, `SIGCHLD` delivery/reaping, exit status, and clean channel
close on the multicore Tribe profile. Optional shell-side diagnostics verify that `ps`
reports the parked `dropbear` PID, `ifconfig -a` reports `eth0` at
`192.168.76.2/24`, and `netstat` reports port 22. After accepting a
connection, Dropbear remains resident across network waits so SSH packets do
not trigger repeated SD-backed executable swaps; UART input can preempt it
back to the interactive shell. BusyBox `ip address`
still requires the unimplemented `AF_NETLINK`/rtnetlink interface; use
`ifconfig -a` for interface inspection in this profile. The regression requires
the paired `dbclient -c none` to match the authorized Ed25519 public key,
complete the test-only transcript proof, and execute a remote marker command;
a transport-only exchange is not considered a pass. The
test-only Dropbear build handles the accepted network connection without a
service fork. Its remote command may perform one nested fork using the
interactive kernel's stacked address-space snapshots. The rootfs `rcS` uses
the same explicit foreground-in-background invocation so a normal init boot
owns and reports the service.

The automated SSH regression starts its single client as soon as the TCP
listen marker appears. Set `TRIBE_INTERACTIVE_SSH_EARLY_CLIENT=0` only when
the slower shell-side `ps`, `ifconfig`, and `netstat` checks are also wanted.

Do not start a second Dropbear after `MIKOS:TCP_LISTEN 22` and
`MIKOS:BACKGROUND_PARK 2`; those markers mean the original service is alive.
The interactive kernel rejects a duplicate execution with `ETXTBSY` instead
of suspending the shell through another slow initialization. Restart Dropbear
only after `MIKOS:BACKGROUND_EXIT` reports that the parked service ended.

The MikOS test build also enables Dropbear's test-only `none` cipher. A
Dropbear client built with the same `DROPBEAR_NONE_CIPHER=1` option can request
an unencrypted, unauthenticated SSH test transport:

```sh
ip neigh flush to 192.168.76.2 dev tap-tribe 2>/dev/null || true
build/tests/busybox/dropbear-host/dbclient \
  -i build/tests/busybox/dropbear-host/mikos_ssh_key.dropbear \
  -c none -y -y root@192.168.76.2
```

The interactive launcher now flushes the neighbor entry at startup, and the
shown command flushes it again defensively. This avoids an immediate `No route
to host` from stale state left by an earlier run. The client then performs
fresh ARP resolution as part of the connection. Do not gate the client behind
a short ping: one packet can take longer than ping's wall-clock timeout in the
cycle-accurate model.
Use this client as the first SSH connection after Dropbear starts; the current
`DEBUG_NOFORK` test profile accepts one connection.

If `MIKOS:TCP_ACCEPT` is already present, the server's only session has been
claimed (possibly by a client still running in another terminal). A later
dbclient is not a retry of that connection: stop the old client/simulator,
start `tribe_interactive.sh` again, wait for `MIKOS_SSH_STARTING`, and run the
printed command exactly once. If no accept marker exists, inspect
`ip neigh show dev tap-tribe`; a `FAILED` entry can be reset with the printed
`ip neigh flush` command.

The normal BusyBox/rootfs and `tribe_interactive.sh` builds produce this host
client and a converted Dropbear-format copy of the test identity automatically.
Passing the OpenSSH private-key file directly to `dbclient -i` is unsupported;
use the generated `mikos_ssh_key.dropbear`. `make dropbear-client` remains
available as an explicit incremental target.

This is a deliberately insecure cleartext SSH protocol test mode rather than
raw Telnet: host-key and user-public-key state machines and SSH framing still
run, and the configured public key must match `authorized_keys`, but this mode
provides no cryptographic host/user authentication, confidentiality, or packet
MAC. For speed, the paired client/server use a deterministic precomputed
X25519 transcript and a SHA-256 transcript/public-key proof in place of
Ed25519 only when `-c none` is explicitly selected. AES/encrypted transports
retain upstream X25519 and Ed25519. Stock OpenSSH does not implement the
`none` cipher, so `/usr/bin/ssh -c none` cannot be used.

Interactive runs and regressions use `tribe_interactive.sh` as their single
public entry point and use the native C++ Tribe simulator by default. Select an
automated regression with `--test ping`, `--test tcp`, `--test process`, or
`--test ssh`. Pass `--verilator` explicitly when testing the separate
Verilator pipeline.

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
