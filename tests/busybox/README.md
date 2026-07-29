# BusyBox compatibility test

This directory owns the user-space workloads used by the current RV32 QEMU
acceptance image. They are tests of MikOS's Linux compatibility surface, not
part of the kernel build implementation.

The build is reproducible from the checked-in BusyBox configuration and pinned
source revisions:

```sh
make -C tests/busybox busybox
make -C tests/busybox stress-ng
make -C tests/busybox payloads
```

`download_busybox.sh` fetches the pinned BusyBox revision into
`build/tests/busybox`. To use an existing checkout without network access:

```sh
make -C tests/busybox \
  BUSYBOX_REFERENCE=/path/to/busybox \
  payloads
```

`build_busybox.sh` performs the out-of-tree cross-build and verifies the ELF
load address. The pinned stress-ng Git submodule and MikOS-only patch live
under `third_party/` and `patches/` here so the complete compatibility test is
self-contained. Initialize the submodule with `git submodule update --init`
after cloning MikOS.
