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
tests/tribe/tribe_interactive.sh --multicore
```

The TAP bridge socket defaults to `/tmp/tribe-ethgig.sock`. The launcher builds
and starts the maintained bridge on `tap-tribe` itself, requesting `sudo` once
when the host TAP must be created. It also replaces an older cpphdl bridge that
uses the same default socket, because that bridge silently drops datagrams on
backpressure and retains stale simulator-peer state. Set `TRIBE_ETH_TAP_SOCKET`
or pass `--tap-socket` only for an intentionally externally managed bridge.
With `tap-tribe` configured as `192.168.76.1/24`, the guest is configured
automatically; it can be pinged from the host:

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
parent/child address-image restoration, nonzero child-status reaping, and a
nested shell fork of external `ls` on the multicore Tribe profile. It does not
claim that parent and child are both runnable; background ordering and
blocking-pipe scheduling remain pending. Its per-marker wall timeout defaults
to two hours because the native cycle-level C++ model can spend tens of minutes
inside a sequence of external BusyBox executions; override it with
`TRIBE_INTERACTIVE_PROCESS_TIMEOUT` when using a faster model. The harness
replaces rcS only in its temporary rootfs copy so it does not spend CPU cycles
starting the unrelated Dropbear service; the normal rootfs remains unchanged.
Set `TRIBE_INTERACTIVE_PROCESS_NESTED_ONLY=1` for the focused shell-to-external
`ls` fork regression; the default remains the complete process sequence.

`make tribe-interactive-ssh-test` configures the guest address, starts
Dropbear with `-F` as a BusyBox background job, waits for the kernel TCP listen
marker, and then verifies a real host SSH connection through `accept`,
identification, key exchange, none-mode test authentication, nested remote
command execution, `SIGCHLD` delivery/reaping, exit status, clean channel
close, and a second independent PTY-backed connection to the same listener on the
multicore Tribe profile. Optional shell-side diagnostics verify that `ps`
reports the parked `dropbear` PID, `ifconfig -a` reports `eth0` at
`192.168.76.2/24`, and `netstat` reports port 22. After accepting a
connection, Dropbear remains resident across network waits so SSH packets do
not trigger repeated SD-backed executable swaps; UART input can preempt it
back to the interactive shell. BusyBox `ip address`
still requires the unimplemented `AF_NETLINK`/rtnetlink interface; use
`ifconfig -a` for interface inspection in this profile. The regression requires
the paired `dbclient -c none` to match the authorized Ed25519 public key,
complete the test-only transcript proof, and execute a remote marker command;
a transport-only exchange is not considered a pass. The test-only Dropbear
build retains the normal service fork. The persistent listener, serialized
SSH session, interactive shell, and external commands use exact-sized stacked
address-space snapshots allocated from the dedicated physical-RAM gap between
the kernel and the first user image. Nesting transfers snapshot ownership
without recopying it; there is no separate 512 KiB or 4 MiB per-process cap.
Fork returns `ENOMEM` only when the shared arena or bounded process metadata is
actually exhausted.
After the command and session are reaped, the listener is restored and accepts
the next connection. The rootfs `rcS` uses the same explicit
foreground-in-background invocation so a normal init boot owns and reports
the service.

The simulated Ethernet ingress keeps 16 distinct frames ahead of an eight-entry
guest DMA RX ring. This covers the unpaused SSH key-exchange ACK/payload/ACK
burst. Identical queued TCP ranges are coalesced; after a frame reaches RGMII,
three repeated copies are also suppressed before one recovery retransmission is
admitted. That bounds real-time host retransmission pressure without preventing
TCP recovery if the guest did lose the delivered frame.

The automated SSH regression starts its first client only after Dropbear has
parked and the restored BusyBox shell prints `MIKOS_SSH_STARTING`. This matches
the manual workflow and prevents an early connection from bypassing the
address-space resume path. `TRIBE_INTERACTIVE_SSH_EARLY_CLIENT=1` retains the
older timing as an explicit diagnostic. Set `TRIBE_INTERACTIVE_SSH_DIAGNOSTICS=1`
to run the slower shell-side `ps`, `ifconfig`, and `netstat` checks before the
late client. By default the regression leaves dbclient running so the KEX
ACK/payload/ACK burst matches the normal manual workflow. Set
`TRIBE_INTERACTIVE_SSH_UART_PREEMPT_PROBE=1` to additionally suspend dbclient
before the first KEX reply and leave UART input pending until
`MIKOS:BACKGROUND_CONNECTION_HOLD 3`; this separately verifies that a nested
SSH child stays resident instead of being incorrectly parked in favor of its
listener.
The second client is a true interactive login shell: it forces `-t`, waits for
the remote prompt through a local PTY, verifies Ctrl+C recovery, prints
`SSH_TTY`, runs `ls /`, and then starts interactive `top`. The shell consumes
those command lines from one batch so the host PTY cannot turn a short command
into several multi-minute executable swaps. `top` still starts only after
`ls` exits, blocks one fork level below the waiting shell, and must relay its
PTY wait through that shell to Dropbear. The harness sends `1`, requires a
rendered memory summary and a second relay, sends `q`, then requires the `top`
zombie to be reaped and the shell to park at a fresh prompt. This catches both
lineage bugs where PPID becomes zero and one-way PTY handoffs that cannot
restore the shell. A single Ctrl+D at that empty prompt performs clean logout
without dbclient splitting a longer `exit` line into expensive character-sized
handoffs. The test also requires `/proc/self/fd/<n>` resolution and
`/dev/pts/<n>` identity checks. `TRIBE_INTERACTIVE_SSH_PTY_ONLY=1` runs this
interactive PTY exchange as the first and only session for a shorter focused
regression.

Do not start a second Dropbear after `MIKOS:TCP_LISTEN 22` and
`MIKOS:BACKGROUND_PARK 2`; those markers mean the original service is alive.
The interactive kernel rejects a duplicate execution with `ETXTBSY` instead
of suspending the shell through another slow initialization. Restart Dropbear
only after `MIKOS:BACKGROUND_EXIT` reports that the parked service ended.

The MikOS test build also enables Dropbear's test-only `none` cipher. A
Dropbear client built with the same `DROPBEAR_NONE_CIPHER=1` option can request
an unencrypted, unauthenticated SSH test transport:

```sh
build/tests/busybox/dropbear-host/dbclient \
  -i build/tests/busybox/dropbear-host/mikos_ssh_key.dropbear \
  -c none -y -y root@192.168.76.2
```

Once connected, `Ctrl+C` is a remote PTY interrupt; it does not terminate the
local dbclient. Run `exit` to log out cleanly, or type Dropbear's `~.` escape at
the beginning of a line to force the client connection closed.

Run that command without a short wall-clock wrapper. On the current native
cycle-level multicore model, reaching public-key authentication can take about
25 minutes and a command session about 35 minutes. In particular,
`timeout 1000 dbclient ...` terminates the peer during key exchange; the guest
then correctly reports `Exit before auth`. The explicit `-i` argument is also
required: dbclient does not discover the generated MikOS Dropbear-format key
from its normal per-user default path.

ARP resolution can take longer than Linux's neighbor-probe timeout in the
cycle-level model. The interactive launcher therefore installs the fixed
guest mapping `192.168.76.2 -> 02:00:00:00:00:02` as a permanent neighbor
entry before boot. Configuring a host neighbor requires `CAP_NET_ADMIN`; for a
normal interactive terminal the launcher invokes `sudo` when the mapping is
missing. In a noninteractive environment it prints the exact privileged
command and continues with ARP fallback, preserving regressions that use an
already-managed TAP. It no longer hides a failed `ip neigh flush` and later
reports `No route to host`. Do not replace the permanent mapping with an ARP
flush or gate the client behind a short ping.
Sessions are serialized by the flat address-space scheduler. Exit the current
dbclient and allow its session child to close and be reaped before starting
another; the listener is then restored and accepts the next connection. If no
accept marker exists, `ip neigh show to 192.168.76.2 dev tap-tribe` must show
the fixed MAC with state `PERMANENT`.

The host TAP may emit traffic according to wall time while only a few guest
cycles execute. MikOS currently implements ARP and IPv4, so the maintained
bridge and native Tribe adapter reject other EtherTypes before their bounded
queues. Both queues preserve TCP payload/SYN/FIN/RST and ARP ahead of
recoverable pure ACKs, retain packets on Unix-datagram backpressure, and clear
queued traffic when a new simulator sends HELLO. The default bridge also
disables IPv6 on `tap-tribe`; externally managed bridges do not get modified.

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
