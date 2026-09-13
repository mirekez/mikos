# Tribe fixes in cpphdl

Tribe tests now require `export CPPHDL_HOME="$HOME/cpphdl"`. Fixes are maintained
in that checkout, including its working-tree changes, rather than applied by
mikOS at build time. The old pinned clone and `tests/tribe/patches` are retired.
A checkout used elsewhere must contain the fixes below. The current local
checkout includes them in commit `3e739c3` (subsequently merged into `f1a1f7f`).

## Network failure causes

The harness used the sticky RX interrupt as a cache-invalidation request.
Single-core L1 initialization restarted continuously while that interrupt was
pending. The harness now uses an RX descriptor write-completion event,
independent of interrupt enable, and retains it until the CPU can invalidate
between memory requests. Repeated descriptors work even while IOC stays high.
Readiness is now a functional CPU port, checked across every core, rather than
a debug value that returned zero when MMU support was disabled. That old check
allowed invalidation of a held load response and occasionally corrupted copied
packet bytes. A small real-driver probe reproduced the corruption before this
change and passed 20 consecutive 512-byte ping runs afterward.

When the guest stopped draining RX, the RGMII PHY could lose nibble alignment
and frame termination under backpressure. The MAC could then fill its frame
buffer and hold ready low forever, preventing recovery on subsequent ICMP
requests. The PHY now discards an overflowing frame while preserving framing
and terminates the partial downstream packet. The MAC drains oversized frames
through their end marker. Normal traffic can resume after the guest drains RX.

The 512-byte regression also exposed early test termination: the UART success
marker can precede transmission of buffered Ethernet bytes. The harness now
waits for the MAC, PCS, PHY, and host TX queue to drain before ending a
successful socket-backed test. The mikOS runner also allows the peer to publish
its verdict after receiving the final datagram. The peer drains queued replies
before retrying: a valid echo may follow an ARP reply in the receive queue after
the simulator socket has closed. `make tribe-peer-test` checks both valid and
corrupt queued replies and is included in `make test`.

The rootless mikOS peer checks ARP and an ICMP reply with a 512-byte payload,
including addresses, echo identifiers, lengths, checksums, and payload bytes.
This also exercises reception beyond the old 256-byte MAC configuration.

## Former patch disposition

| Former patch | Disposition in current cpphdl |
| --- | --- |
| `current-cpphdl-no-mmu` | Feature guard already present; retained no-MMU compile regression. |
| `no-isr-csr-time` | ISR guards already present; migrated the missing CMake stacktrace-capability guard. |
| `native-port-cache-fast-path` | Already present upstream. |
| `polling-dma-invalidate` | Replaced with completion-event invalidation; pulsing a sticky IRQ still repeatedly invalidates the cache. |
| `preserve-host-control-frames` | Removed simulated-time ARP/ICMP suppression. |
| `tcp-rx-burst-backlog`, `reliable-eth-queues` | Migrated bounded queues, preservation of an in-flight frame, priority protection, and TX dequeue only after successful socket send. The mikOS-specific IPv4/ARP filter was omitted. |
| `ssh-kex-rx-burst-capacity` | Migrated the 16-frame queue capacity. |
| `coalesce-tcp-ingress` | Migrated preservation of earlier queued payload. Omitted semantic TCP hashing: distinct packets must not be treated as identical merely because their headers match. |
| `coalesce-completed-tcp-ingress` | Omitted: suppressing retries after RGMII transmission can suppress recovery when the guest never received the frame. |
| `full-ethernet-frame-rx` | Migrated the 2048-byte MAC configuration. |
| `verilator-memory-config` | Migrated RAM/IO sizing and multicore wrapper configuration. |
| `cache-eth-dma-trace-toggle` | Migrated cached trace-option lookup. |
| `multicore-clint-hart0` | Retired the callback-bypass workaround. Current per-hart ports are indexed correctly; no current timer failure was established. |

## Regression commands

```sh
export CPPHDL_HOME="$HOME/cpphdl"
export RISCV_HOME="$HOME/riscv"
bash "$CPPHDL_HOME/tribe_cpu/tests/network_regression.sh"
make tribe-kernel-test
bash tests/tribe/prepare_cpphdl.sh --multicore
bash tests/tribe/run_kernel.sh --multicore
make tribe-all-tests
```

The cpphdl regressions cover RX completions with a held/masked IRQ, descriptor
rings on 64/256-bit buses, programmed MAC registers, oversized-frame and burst
overflow recovery, RX queue ordering, TX retention, large-frame CPU loopback, no-MMU polling copies during DMA, and the build
with MMU/interrupts/atomics disabled. The suite above uses native simulation;
it does not claim a Verilator end-to-end run.
