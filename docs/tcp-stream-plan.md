# Passive IPv4 TCP and stream-socket implementation plan

## Goal and boundary

The first stream slice gives flat RV32 workloads a real passive IPv4 TCP
transport. It is intentionally allocation-free and polling-only, matching the
rest of MikOS. The completed slice includes `SOCK_STREAM`, `bind`, `listen`,
`accept`/`accept4`, accepted-socket `read`/`write`, readiness polling, endpoint
queries, shutdown/close, and `/proc/net/tcp` visibility.

This is a passive, bounded transport rather than a general TCP/IP stack. It
does not claim active `connect`, IPv6, IP fragmentation, TCP congestion
control, adaptive retransmission timers, unbounded reassembly, urgent data, window
scaling, or unbounded queues. Those features must be added only with a caller
and regression that needs them.

## Implementation phases

1. **ABI and lifecycle — complete.** Add asm-generic RV32 syscall numbers,
   validate stream/datagram type and TCP protocol combinations, define the
   16-byte IPv4 socket address, and preserve socket references across
   descriptor duplication and close.
2. **Bound socket table — complete.** Use thirteen static slots, four pending
   connections per listener, 4 KiB receive queues, explicit lifecycle states,
   wildcard/exact bind collision rules, and deterministic ephemeral ports.
3. **Passive TCP — complete.** Validate Ethernet/IPv4/TCP lengths and
   checksums, reject fragments, perform SYN/SYN-ACK/ACK establishment, handle
   duplicate SYNs, assemble bounded out-of-order payload through an
   open-addressed hash keyed by socket and sequence number, acknowledge FIN,
   surface RST, and emit FIN on shutdown/last close.
   Retain a bounded set of outbound payload segments keyed by socket and TCP
   sequence, retire them through cumulative ACKs, and retransmit silently lost
   payload from the polling loop.
4. **Syscall I/O — complete.** Implement blocking/nonblocking accept and read,
   bounded segmented writes, `ppoll` and `pselect6` readiness, harmless socket
   option hints used by servers, endpoint queries, and shutdown.
5. **Observability — complete.** Generate Linux-shaped `/proc/net/tcp` rows for
   listening, half-open, established, and close-wait sockets so BusyBox
   `netstat` reports the actual state.
6. **Target acceptance — complete on the configured Tribe TAP.** Start
   BusyBox `nc`, connect through the host TCP stack, execute `/bin/sh` on the
   accepted stream, transfer a marker into the guest filesystem, and verify it
   over UART with `make tribe-interactive-tcp-test`.
7. **SSH service follow-up — pending outside this socket slice.** Booting
   Dropbear also needs the long-lived process model: init/service execution,
   ordinary `fork`, pipes, signal delivery/reaping, concurrent parent/child
   address spaces, and PTY/session support. Socket success must not be reported
   as SSH success until those gates pass.

## Rational regression inventory

The list is exhaustive for behavior this bounded slice promises. It avoids
combinatorial tests for features explicitly outside the boundary.

### ABI and syscall contract

- Verify RV32 numbers for `socket`, `bind`, `listen`, `accept`, `accept4`,
  `pselect6`, and `pselect6_time64`.
- Accept IPv4 datagram protocol zero, IPv4 stream protocol zero, and explicit
  TCP protocol six.
- Accept `SOCK_NONBLOCK` and `SOCK_CLOEXEC`; reject unknown type bits.
- Reject non-IPv4 families and protocol/type mismatches.
- Require a complete, readable `sockaddr_in`; reject a wrong family, short
  length, bad pointer, invalid descriptor, and non-socket descriptor.
- Reject bind to an address not owned by the configured interface.
- Reject unsupported `accept4` flags and malformed peer-address outputs.
- Verify blocking accept polls, nonblocking accept returns `EAGAIN`, and an
  accepted socket receives only explicitly requested status flags.
- Verify `getsockname` on bound/listening/accepted sockets and reject
  `getpeername` before connection establishment.
- Verify zero-length I/O, EOF, reset/error, shutdown directions, and bad
  shutdown values.
- Verify duplicate descriptors retain one socket, replacement releases the
  overwritten socket, and only the last close tears down transport state.

### Socket-state invariants

- A datagram control socket cannot bind as a TCP listener.
- `listen` before bind fails; repeated listen updates a clamped backlog.
- Wildcard binds conflict with every exact address on the same port; distinct
  exact addresses can share a port at the table layer.
- Port zero allocates unique deterministic ephemeral ports.
- A duplicate SYN maps to the existing half-open child and preserves its
  original server sequence number.
- A wrong handshake ACK cannot establish; the exact ACK can.
- Backlog saturation and total table exhaustion fail without corrupting
  existing sockets.
- Accept returns each established child once and preserves its peer endpoint.
- Closing a listener releases unaccepted children but not already accepted
  descriptors.
- Ordered receive advances the expected sequence, partial application reads
  compact the queue, empty established reads would block, FIN becomes EOF
  after queued data, `CLOSE_WAIT` remains writable until local shutdown, and
  RST becomes readable error state.
- Out-of-order segments occupy a fixed open-addressed hash entry keyed by
  socket and sequence number; filling a gap drains every now-contiguous entry,
  duplicate keys do not consume capacity, and table exhaustion is explicit.
- Receive capacity never overflows; excess bytes are not acknowledged as
  consumed and can be retransmitted.
- Outbound payload writes occupy distinct fixed hash entries keyed by socket
  and sequence number; cumulative ACKs retire every covered entry.
- A failed initial NIC transmit is reported without leaving a ghost retry;
  an unacknowledged successful transmit is retried after the fixed polling
  interval; reset and final close clear all retained payload.

### Wire validation and response generation

- Accept a valid Ethernet/IPv4/TCP frame with valid IPv4 and pseudo-header TCP
  checksums.
- Reject truncated Ethernet/IP/TCP headers, impossible total/header lengths,
  non-IPv4 versions, non-TCP protocols, wrong destination addresses, damaged
  IPv4 checksums, damaged TCP/payload checksums, and fragmented IPv4 packets.
- Parse legal IPv4 and TCP options by their header lengths while ignoring
  unsupported option semantics safely.
- Emit a SYN-ACK with reversed MAC/IP/ports, ACK equal to client sequence plus
  one, a stable server sequence on duplicate SYN, a bounded receive window,
  and valid checksums.
- Ignore ACKs that cannot establish a half-open connection.
- Accept only the expected payload sequence, ACK newly accepted bytes, and
  issue a duplicate ACK for retransmitted/out-of-order input.
- Segment application writes below the NIC frame bound with monotonically
  increasing sequence numbers and valid checksums.
- Retransmit an unacknowledged segment with its original sequence and payload;
  acknowledge adjacent writes independently and preserve only the uncovered
  suffix after a cumulative ACK.
- ACK FIN at sequence plus one; surface EOF while preserving the writable
  local half; emit local FIN exactly once on shutdown or last close; surface
  RST without returning stale data.
- Silently discard malformed packets and SYNs to closed ports in this slice.

### Cross-layer regressions implemented

- Fake-NIC regression: open/bind/listen, inject SYN, validate SYN-ACK, inject
  ACK, accept, transfer data in both directions, inject FIN, read EOF, close,
  and validate the final FIN.
- Confirm a listener appears in `/proc/net/tcp` with state `0A` and its bound
  port in network notation.
- Re-run existing ARP, ICMP echo, UDP-control ioctl, filesystem, scheduler,
  and both NIC compile checks to catch regressions outside TCP.
- Build normal, Tribe, interactive Tribe, and multicore interactive kernels
  with freestanding warnings treated as errors.
- Target acceptance runner: configure `eth0`, run BusyBox `nc -l`, wait for the
  kernel's deterministic `MIKOS:TCP_LISTEN <port>` milestone, connect one
  long-lived host client bound to the TAP address, transfer a unique marker,
  close cleanly, verify the marker from the guest filesystem, and exit MikOS
  normally. A single client tuple is important on a cycle-accurate target:
  short host retries would test SYN-expiry policy rather than stream I/O.
  BusyBox's nc-1.10-compatible applet execs `/bin/sh` on the accepted socket;
  the host sends a command that writes the marker and exits. This avoids the
  relay variant's intentional wait for interactive UART stdin to reach EOF.

### Target follow-up regressions planned

- Closed port does not accept, bad checksum produces no
  response, second connection beyond backlog is not accepted, and listener
  close removes the `/proc/net/tcp` row.

## Completion criteria

The socket slice is complete when all deterministic host regressions pass,
all kernel profiles compile, and the environment-backed Tribe TCP runner
passes on a configured TAP bridge. SSH remains a separate completion gate and
must include an actual authenticated command over Dropbear.
