# `/proc` and `/sys` regression inventory

Implemented in `tests/kernel/pseudo_filesystem_test.cpp`:

- `/proc` and `/sys` mount boundaries do not match similarly prefixed paths.
- BusyBox `netstat` inputs `/proc/net/tcp`, `tcp6`, `udp`, `udp6`, and `raw`
  resolve to regular files.
- `/proc/net` enumerates its socket tables with the correct file types.
- Internet socket tables contain the Linux `local_address` header expected by
  BusyBox and report a nonzero size.
- The partial sysfs tree resolves `/sys/devices/system/cpu/online` and
  enumerates CPU topology files.
- `/sys/kernel/ostype` exposes the expected Linux compatibility value.

Covered by the interactive Tribe check:

- `netstat -ln` reads every configured internet socket table without a
  missing-file diagnostic.
- `/proc/net` can be listed from BusyBox through `getdents64`.
- `/sys/devices/system/cpu/online` reports the multicore topology through the
  normal BusyBox open/read path.
