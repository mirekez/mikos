# RV32 stress-ng integration

Mikos bundles BusyBox and a static RV32 stress-ng in the same kernel image. The
boot acceptance test runs them sequentially in the single flat user region:

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
remains unmodified. `make kernel` requests both test payloads, converts them to
linkable binary objects, and embeds them in `mikos-rv32.elf` for the current
acceptance image.

Run the complete acceptance path with:

```sh
export PATH="$PWD/.conda/bin:$PATH"
make test
make qemu-test
```

## Supported scope

The `rv32-flat` profile has one process and intentionally provides no `fork`,
filesystem, or general signal delivery. Normal stress-ng launches every worker
with `fork`, so the build-only `STRESS_MIKOS` path runs the one requested worker
in-process while retaining upstream option parsing, setup, CPU stressor,
verification, metrics, status accounting, and cleanup. It is enabled only for
the Mikos build and does not alter stress-ng's normal launcher.

This is a real CPU-stressor compatibility test, not a claim that the full
stress-ng suite works. Stressors requiring processes, threads, filesystems,
devices, networking, `/proc`, or signal delivery remain outside this POC's
supported behavior. Unsupported discovery calls return Linux errors and remain
visible as `MIKOS:ENOSYS` trace records.
