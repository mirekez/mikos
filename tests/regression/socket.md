# Socket and passive TCP regression inventory

Implemented in `tests/kernel/socket_syscall_test.cpp`:

- RV32 uses Linux asm-generic syscall number 198.
- `socket(AF_INET, SOCK_DGRAM, 0)` is accepted for BusyBox `ifconfig`.
- `socket(AF_INET, SOCK_STREAM, 0)` and explicit `IPPROTO_TCP` are accepted.
- `SOCK_NONBLOCK` and `SOCK_CLOEXEC` type flags are accepted.
- Unsupported address families are rejected with the address-family result.
- Unsupported socket types and mismatched protocols are rejected.
- Nonzero protocols are rejected with the protocol result.

Covered by the interactive Tribe ping regression:

- A successful socket receives a normal MikOS file descriptor.
- The descriptor can be passed to interface ioctls and closed by BusyBox.
- Reconfiguring `eth0` through that descriptor affects ARP and ICMP handling.

Covered by `tests/net/socket_test.cpp`, `tests/net/tcp_test.cpp`, and
`tests/net/stack_test.cpp`:

- outbound TCP payload retention and retransmission after silent loss;
- sequence-keyed adjacent writes, cumulative ACK retirement, and cleanup on
  the final socket close;
- fixed-capacity bind/listen/accept lifecycle, conflict and backlog rules,
  handshake transitions, reference counts, ordered receive, EOF, and capacity;
- IPv4/TCP length, fragment, destination, and checksum validation;
- a complete fake-NIC SYN/SYN-ACK/ACK exchange, bidirectional stream payload,
  FIN/EOF, post-FIN local-half write, close, and live `/proc/net/tcp` listener
  reporting.

Covered by `make tribe-interactive-tcp-test` when `tap-tribe` is configured:

- an unmodified RV32 BusyBox `nc` uses the actual syscall dispatcher to bind,
  listen, accept, receive a host TCP stream, persist its payload, and exit. The
  runner waits for `MIKOS:TCP_LISTEN <port>` and uses one long-lived host
  connection so cycle-accurate execution cannot manufacture backlog pressure
  through retries. nc execs `/bin/sh` on the accepted stream, so the remote
  command also regresses socket duplication to standard input/output,
  redirected reads, the marker-file write, and clean EOF/exit handling without
  relying on the nc-1.10 relay's never-ending interactive UART stdin.

The complete rationale and negative-case matrix is maintained in
`doc/tcp-stream-plan.md`.
