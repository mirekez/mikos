# RV32 stress-ng integration

Mikos stores BusyBox and a static RV32 stress-ng in one ext4 root image. The
kernel mounts that image read-write and runs `/bin/busybox` and
`/bin/stress-ng` sequentially in the single flat user region:

```text
busybox echo MIKOS_BUSYBOX_OK
stress-ng --cpu 1 --cpu-method loop --cpu-ops 4 --verify --metrics-brief
```

The test requires stress-ng's own CPU metric to report four bogo operations,
its status summary to report one passed CPU worker, its successful-completion
message, a zero exit status, nonzero timer preemptions, and no timer-contract
violations.

## Source and build

The upstream source is a Git submodule at
`tests/busybox/third_party/stress-ng`, cloned from
`https://github.com/ColinIanKing/stress-ng` and is pinned by the checked-out
commit. The integration was developed against commit
`f73c6bd5f34df31dbaea8215e1e9c3f5cbc8d060` (stress-ng 0.21.04).

`make stress-ng` delegates to the BusyBox test Makefile, archives that checkout
into `build/tests/busybox/stress-ng-source`, applies
`tests/busybox/patches/stress-ng-mikos.patch` to the build copy, and creates a
stripped, static RV32 Linux ELF linked at `0x81000000`. The upstream checkout
remains unmodified. `make kernel` requests
`build/tests/busybox/rootfs.ext4`; neither executable is converted to a linker
object or embedded in `mikos-rv32.elf`.

QEMU reads the image through a polling modern virtio-blk driver and Tribe reads
the same sectors through its SD DMA controller. The interactive shell resolves
ordinary absolute and relative paths through that ext4 volume and can execute
static RV32 ELF files found there. When a child exits, BusyBox is restored from
the ext4 file before the saved writable process state is applied. Launch a
session with `tests/tribe/tribe_interactive.sh`; `cd /bin` followed by
`./stress-ng ...` therefore follows the same path semantics as Linux.

Run the complete acceptance path with:

```sh
export PATH="$PWD/.conda/bin:$PATH"
make test
make qemu-test
```

## Supported scope

The `rv32-flat` stress acceptance still runs one worker in place. The
interactive profile now has a bounded serial fork/exec/wait adapter, but no
concurrent runnable process scheduler, handler-frame signal delivery, or every
Linux filesystem feature. Its root
filesystem is a generic writable ext4 mount for the deliberately journal-free,
metadata-checksum-free test format. Normal stress-ng launches every worker with
`fork`, so the build-only `STRESS_MIKOS` path runs the one requested worker
in-process while retaining upstream option parsing, setup, CPU stressor,
verification, metrics, status accounting, and cleanup. It is enabled only for
the Mikos build and does not alter stress-ng's normal launcher.

This is a real CPU-stressor compatibility test, not a claim that the full
stress-ng suite works. Stressors requiring concurrent processes, threads,
unimplemented filesystem/device behavior, or handler-frame signal delivery
remain outside this POC's supported behavior. Unsupported discovery calls
return Linux errors and remain visible as `MIKOS:ENOSYS` trace records.
