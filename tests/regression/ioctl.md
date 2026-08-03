# `ioctl` syscall regression inventory

Implemented in `tests/kernel/ioctl_syscall_test.cpp`:

- RV32 Linux `ifreq` and `sockaddr` sizes and field layout.
- `SIOCGIFINDEX` returns index 1 for `eth0`.
- `SIOCGIFFLAGS` and `SIOCSIFFLAGS` read and update link flags.
- `SIOCGIFADDR` and `SIOCSIFADDR` read and update the IPv4 address.
- `SIOCGIFNETMASK` and `SIOCSIFNETMASK` read and update the netmask.
- Address or netmask changes recompute the broadcast address.
- `SIOCGIFBRDADDR` and `SIOCSIFBRDADDR` read and update broadcast state.
- `SIOCGIFHWADDR` reports the driver MAC with `ARPHRD_ETHER`.
- Non-IPv4 setters, unknown interfaces, and unknown requests are rejected.

Covered by the interactive Tribe ping regression:

- BusyBox can perform its `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCGIFFLAGS`,
  and `SIOCSIFFLAGS` sequence for `ifconfig eth0 ADDRESS netmask MASK up`.
- The new address is used by both ARP and ICMP echo replies.
