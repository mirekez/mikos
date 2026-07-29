# Linux Reference Baseline

## Selected source

The initial Linux ABI and behavioral reference is:

| Field | Value |
|---|---|
| Release from `Makefile` | 6.19.0 |
| Git commit | `05f7e89ab9731565d8a62e3b5d1ec206485eeb0b` |
| Git description observed | `v6.19-dirty` |

## Authoritative syscall-number inputs

| ABI | Linux input | SHA-256 |
|---|---|---|
| i386 | `arch/x86/entry/syscalls/syscall_32.tbl` | `17047f5b9d779f25476aba1f3150a4a15f42cb20a0f87b82a59c81c9e69e1799` |
| x86-64 | `arch/x86/entry/syscalls/syscall_64.tbl` | `b29be7abdb0b12221d927e8b0c64a68b59138dc4e358cee4ecd02a77e71d580e` |
| riscv32 | `scripts/syscall.tbl` plus RISC-V ABI filters | `6dbbeeeaa61d22affdf4d77807f71b8be9a0273fe0cd0b78ad48469e5a6b9aa2` |
| riscv64 | `scripts/syscall.tbl` plus RISC-V ABI filters | `6dbbeeeaa61d22affdf4d77807f71b8be9a0273fe0cd0b78ad48469e5a6b9aa2` |

RISC-V selects entries using
`arch/riscv/kernel/Makefile.syscalls`, whose observed SHA-256 is
`39fcdb4a5cee942d6d79f4db56a75926ebc5aa3f9096eefd9348e2470462c920`.
The public width-selection header is
`arch/riscv/include/uapi/asm/unistd.h`, whose observed SHA-256 is
`0134f2d3cec62a5be0028498ae86408a513b734036afd2d845e239a2079f59e3`.

Generated files under `arch/*/include/generated` are verification artifacts,
not the primary import source.

## How the reference may be used

Use the Linux tree for:

- syscall numbers, ABI names, register conventions, UAPI type layouts, flags,
  constants, error values, and signal-frame definitions;
- understanding edge cases and ordering rules before writing tests;
- creating black-box differential fixtures by running equivalent calls on
  Linux;
- identifying relevant upstream selftests and Linux Test Project coverage.

Do not use Linux as an architectural template for page tables, interrupt-driven
drivers, preemptive scheduling, or in-kernel policy. Those mechanisms conflict
with MikeOS requirements.

Behavioral compatibility tests should prefer observable inputs and outputs over
copying implementation code. Imported UAPI files and generated derivatives must
preserve applicable SPDX identifiers, including the Linux syscall-note where it
applies. Any direct implementation-code reuse requires a separate licensing and
provenance decision.

## Planned regeneration contract

The future importer should behave conceptually as:

```text
generate-syscalls --linux-source PATH --expected-commit COMMIT --manifest FILE
```

It must:

1. verify the selected relative input paths;
2. verify their hashes and report whether unrelated checkout files are dirty;
3. parse ABI rows without depending on Linux build output;
4. emit deterministic C++ metadata, dispatch declarations, and support-manifest
   skeletons;
5. compare generated numbers with Linux generated UAPI headers when available;
6. write the Linux release, commit, relative inputs, and hashes into generated
   provenance;
7. refuse an ABI-source change until the checked-in manifest and pin are updated
   explicitly.

