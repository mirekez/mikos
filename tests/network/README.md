# TCP acceptance suite

This suite links the **production** `network/stack.cpp` and socket implementation
against an in-memory Ethernet device and a controllable architecture clock. It
requires neither Linux userspace nor a RISC-V simulator. Every scenario runs in
a fresh process; there are no reset hooks or test-only protocol branches in the
kernel. Packet encoding and checksum verification are independent of the
production parser. Every outgoing packet is checked for valid lengths and both
checksums before a test can inspect it.

```sh
make -C tests network-test
make -C tests network-sanitize     # AddressSanitizer + UndefinedBehaviorSanitizer
make -C tests test                 # Includes this suite and the older regressions

build/tests/regression/network/tcp_stack_test --list
build/tests/regression/network/tcp_stack_test window/sparse_loss
```

An additional interoperability test uses the host Linux TCP stack:

```sh
make -C tests network-linux-peer
```

It requires the existing `tap-tribe` bridge at `/tmp/tribe-ethgig.sock`, host
`192.168.76.1/24`, and the permanent neighbor for `192.168.76.2` described in
[the Tribe README](../tribe/README.md). Run it while that bridge is not serving
a CPU simulator: its Ethernet peer is temporarily the host-built production
stack. It does not change TAP configuration. Three consecutive sessions must
echo 64,000, 256,000, and 64,000 bytes exactly, with a small Linux receive buffer,
a 1.5-second read pause, half-close and EOF. This checks option interoperability
and actual Linux window/ACK behavior, independently of the deterministic peer.


The suite has 61 named scenarios. Its 12 deterministic stress seeds each transfer
64,000 bytes with packet loss, duplication of acknowledgements, and reordering.
`window/*` also transfers 40,000 bytes per fast, slow, intermittent, bursty, and
sparsely lossy profile. A failing assertion prints its scenario and source line.
Each scenario has a 20-second host watchdog; protocol deadlines use virtual
clock ticks and never sleep. Individual scenarios can be run under a debugger.

## Acceptance criteria

### Windows and delivery

* Fast peers use large windows; slow peers advertise 37 bytes and delay ACKs.
  Intermittent/bursty peers repeatedly close and reopen their windows. All five
  profiles must deliver the exact source bytes and finish within 2,000 rounds.
* Newly transmitted bytes never exceed the remaining peer window **or** the
  congestion window. Zero window returns `would_block` without consuming a
  sequence number. Reopening it restores write readiness.
* A stale ACK, future ACK, or stale window-update sequence must not release
  pending data or enlarge the send window. Partial ACKs remove only their
  acknowledged prefix; retransmission respects a subsequently shrunken window.
* Reads that free receive space schedule a window update. A full 4,096-byte
  receive buffer advertises zero, does not acknowledge unbuffered bytes, and
  resumes after application reads.
* Duplicate, reordered, overlapping and wrapping sequence numbers yield each
  byte exactly once and in order. Bytes past the right receive-window edge must
  not enter reassembly storage. Traffic after received FIN cannot create data.
* The seeded stress series must deliver every byte without corruption or a
  connection failure within 20,000 rounds per seed. Retransmitted bytes must
  match the original source, and sequence numbers must refer only to bytes the
  application successfully wrote.

### Options and congestion control

* Peer MSS constrains every data segment, including the one-byte and 65,535-byte
  edge values; local segments are capped at 1,024 bytes. An absent IPv4 MSS
  defaults to 536. SYN-ACK advertises a receive MSS of 1,460.
* Window scaling is negotiated only by SYN exchange. SYN windows are unscaled;
  subsequent peer windows use its shift, capped at 14. The local shift is zero
  because the bounded receive buffer fits an unscaled window. Midstream options
  cannot renegotiate either MSS or scaling.
* EOL/NOP and well-framed unknown extensions are accepted. Bad option lengths,
  truncated options, zero MSS, and duplicate MSS/scale options are rejected
  before allocating a connection. Timestamps and SACK are not negotiated;
  correctly framed offers must still allow ordinary TCP interoperability.
* Initial congestion window is four effective MSS. ACK progress grows it by at
  most one MSS in slow start; congestion avoidance uses acknowledged-byte
  credit. Three qualifying duplicate ACKs retransmit the oldest segment without
  waiting for RTO. Fast recovery reduces the window; RTO restarts at one MSS.
  An idle sender restarts conservatively. The transmit-storage capacity remains
  an additional bound, never permission to bypass flow/congestion control.

### Timeouts and clock boundaries

All intervals below are guest time. The architecture clock uses 10,000,000 ticks
per second, matching the kernel clock ABI. Tests check just before and exactly
at expiry, a stalled clock under 100,000 polls, and unsigned 64-bit clock wrap.

| Event | Policy and required observation |
| --- | --- |
| Initial data RTO | 1 second; no earlier packet, one oldest-segment retry at expiry |
| Backoff | Exponential, capped at 60 seconds; no per-poll retransmission storms |
| RTT estimation | First 800 ms sample yields 2,400 ms RTO; retransmitted samples do not contaminate the estimate (Karn) |
| ACK at deadline | Incoming ACK is processed before expiry; covered data is never retransmitted |
| Silent data connection | 120 seconds without progress produces `timed_out`/`ETIMEDOUT`, clears transmission/reassembly storage and stops write readiness |
| Half-open SYN | SYN-ACK is retried; the slot expires at 120 seconds even under repeated duplicate SYNs or a permanently busy NIC |
| Zero-window persist | Window queries back off from 1 second to 60 seconds; responsive zero-window peers remain alive, silent peers time out after 120 seconds |
| FIN retransmission | FIN remains available until ACK; retries stop after ACK; close preserves unacknowledged data before sending FIN |
| FIN completion | Uncompleted close expires after 120 seconds; the last application close detaches transport ownership instead of freeing live retransmission data |
| FIN-WAIT-2 | Detached connections waiting for peer FIN expire after 120 seconds |
| TIME-WAIT | Retain the tuple for 120 seconds (2 x 60-second MSL); duplicate FIN is acknowledged and restarts the interval; RST cannot terminate it early |
| Driver refusal | No sequence-number consumption on failed initial data send; timer retries are deferred, and connection expiry still works |
| Healthy idle | No implicit idle disconnect: a connection remains usable after 24 virtual hours |

Keepalive is disabled and ACKs are immediate, so there is no enabled keepalive
or delayed-ACK deadline to simulate. Persist uses an old-sequence ACK query when
there is no retained application byte; when data is outstanding, retransmission
can probe one retained byte at a zero peer window. Successful writes own their
payload; unsuccessful/partial writes leave the remainder with the caller.

### Floods, capacity and cleanup

* Socket capacity is 13 (including listeners), listener backlog 4, pending
  transmit segments 16, reassembly segments 16, and receive storage 4,096 bytes
  per socket. Full structures return bounded backpressure without advancing
  sequence numbers, overwriting stored data, allocating heap storage, or
  claiming writability while transmit storage is full.
* Filling and draining every transmit slot is repeated 20 times. Reassembly
  overflow is recovered through retransmission and repeated 10 times. Reset
  cleanup fills transmit/reassembly storage and reconnects 40 times.
* SYN floods cannot exceed backlog; completed-but-unaccepted reset floods leave
  nothing for `accept`. Closing a listener removes its unaccepted children and
  pending control timers; accepted connections remain independently owned.
* A 1,000-packet far-future sequence flood must not steal reassembly capacity.
  A 1,000-packet duplicate-data flood delivers one copy. A 10,000-packet checksum
  corruption flood emits no reply and leaves the established connection usable.
* An exact receive-sequence RST terminates the connection; out-of-window RSTs
  cannot. In-window but non-exact RSTs elicit at most 100 challenge ACKs per guest
  second. Invalid SYN flag combinations cannot allocate backlog entries.
* Closed ports return a reset with the correct sequence/acknowledgement, never
  answer another reset, and share the 100-per-second control-response limit.
* Two independent flows must both receive a retry when due; ACKing one must not
  clear or reorder the other's transmission. Slot reuse after termination must
  never retransmit an earlier connection's bytes.
* A peer FIN preserves the ability to send a response, duplicate FIN never
  advances ACK twice, final FIN ACK releases LAST-ACK, and attached as well as
  detached TIME-WAIT sockets can be cleaned up.

## Scope and completion record

The acceptance target is mikOS's bounded, passive-open IPv4 TCP transport used
by BusyBox `nc` and Dropbear. This is not a claim of complete Internet TCP or a
replacement for NIC/simulator integration tests. Active-open `connect`, IP
fragment reassembly, SACK, timestamps/PAWS, and configurable keepalive are not
implemented by this transport. Existing Linux-ABI socket-option hints do not
turn those features on.

The first 37 scenarios passed **6/37** against the pre-change implementation.
Failures exposed ignored windows/options, poll-count retries, missing timers,
unsafe reset/handshake acceptance, missing FIN retention, and stale capacity.
The expanded acceptance run passes **61/61**, including ASan/UBSan. The real
Linux peer also passes all three sessions (384,000 echoed bytes). The older
socket/parser/stack tests remain in `tests/net/`; their retry tests now advance
virtual time, and close tests complete the FIN handshake before tuple reuse.

Protocol references: [TCP](https://www.rfc-editor.org/rfc/rfc9293.html),
[retransmission timers](https://www.rfc-editor.org/rfc/rfc6298.html),
[window scaling](https://www.rfc-editor.org/rfc/rfc7323.html), and
[congestion control](https://www.rfc-editor.org/rfc/rfc5681.html).
The 120-second user/handshake/close deadlines and bounded capacities are local
resource policies. Optional extensions are deliberately not advertised.
