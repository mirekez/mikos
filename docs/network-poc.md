# Polling Network POC

## Current boundary

The first network slice implements only what is needed to prove a real packet
can cross the QEMU/guest boundary in both directions:

- `include/mikos/drivers/net.hpp` defines a four-operation, polling-only NIC
  interface: initialize, read MAC, receive one frame, and transmit one frame;
- `include/mikos/drivers/virtio.hpp` owns typed features/status and compile-time
  modern and legacy split-queue state machines;
- `include/mikos/drivers/virtio_net.hpp` is one constrained
  `BasicNetDevice<Transport, Queue, HeaderSize, ReceiveBuffers>` data path used
  by both architectures; transports contain only discovery and register rules;
- `drivers/net/virtio_mmio.cpp` implements modern virtio-net over the QEMU
  RISC-V `virt` MMIO transport;
- `drivers/net/virtio_pci_legacy.cpp` starts the IA-32 transport with PCI
  configuration-port discovery and legacy virtio I/O queues. It is compile
  checked now and will be target-tested when the IA-32 boot profile exists;
- all queues and packet buffers are fixed-size static objects;
- no NIC interrupt or PLIC source is enabled;
- `network/stack.cpp` owns the temporary fixed address `10.0.2.15`, answers ARP
  requests for that address, and contains allocation-free ICMP echo handling;
- `tests/qemu/net_peer.cpp` is a deterministic host Ethernet peer connected
  through QEMU's Unix-datagram backend.

The virtio device advertises many features, but the POC negotiates only
`VIRTIO_F_VERSION_1` and `VIRTIO_NET_F_MAC`. It intentionally omits mergeable
buffers, checksum/GSO offload, multiqueue, control queues, and event-index
notifications. Modern virtio-net still places a 12-byte guest header before a
frame even without the mergeable-buffer feature; this layout is covered by the
end-to-end RX/TX test.

The driver and stack currently execute during boot in kernel context. That is
temporary bootstrap scaffolding, not the intended service boundary. Once the
fixed task table and IPC mechanism exist, the same narrow NIC contract moves to
a user-space driver service, with the privileged kernel limited to granting
the MMIO/DMA region.

## Run the acceptance test

```sh
export PATH="$PWD/.conda/bin:$PATH"
make test
make qemu-net-test
```

The network test waits until the guest has posted receive buffers, injects a
broadcast ARP request for `10.0.2.15`, validates the returned MAC/IP fields,
then sends an IPv4 ICMP echo request and validates both reply checksums. It also
requires these guest markers:

```text
MIKOS:NET_IP 10.0.2.15
MIKOS:NET_MAC 52:54:00:12:34:56
MIKOS:ARP_REPLY
MIKOS:ICMP_ECHO_REPLY
MIKOS:EXIT 0
```

The Unix-datagram backend is used because the supplied QEMU was built without
SLIRP and the host has no available TAP device. It gives deterministic raw
Ethernet tests without root privileges or host network configuration.

## Not supported yet

An ARP reply proves only link-layer RX/TX and address ownership. It does not yet
provide Linux socket syscalls, DHCP, TCP, an SSH server, PTYs, multiple
long-lived processes, or `/proc` data for `top`.

The next acceptance gates are, in order:

1. **Complete:** host receives a valid ICMP echo reply;
2. host completes a minimal TCP exchange;
3. BusyBox `ip addr` reports the fixed address through Linux-compatible APIs;
4. a user-space SSH server accepts one forwarded QEMU connection and starts a
   BusyBox shell;
5. `top` reads bounded process/accounting data;
6. the same tests pass through an x86 polling virtio-net PCI transport.

For its first target test, the IA-32 driver deliberately requires QEMU's
transitional virtio device. It exposes the required 256-entry legacy ring but
posts only eight fixed RX buffers. Avoiding modern PCI capability parsing is
the smaller first implementation; modern virtio PCI can be added only if an
application or machine profile requires it.
