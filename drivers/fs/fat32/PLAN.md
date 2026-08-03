# FAT32 Driver Development Plan

## Contract and sources

The driver is a freestanding C++ component over the repository's
`ReadableDevice` byte-range interface. It does not own storage, allocate memory,
or depend on a hosted runtime. Recoverable failures are explicit `Result<T>`
values.

The on-disk rules come from Microsoft's
[FAT32 File System Specification 1.03](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/fatgen103.doc).
No existing FAT implementation is an input to this work.

## Milestones

### F1 — Validated read-only mount

- Decode little-endian BPB fields without packed-structure aliasing.
- Accept legal 512/1024/2048/4096-byte sectors and power-of-two cluster sizes.
- Derive FAT/data offsets with checked 64-bit arithmetic.
- Confirm the volume is FAT32 by cluster count, not its label string.
- Validate FAT capacity, root cluster, active FAT selection, media bounds,
  version, and boot signature.
- Expose immutable geometry for diagnostics.

Gate: table-driven boot-sector tests cover every accepted sector size and each
invalid or overflowing field independently.

### F2 — Cluster chains and byte reads

- Mask FAT32 entries to 28 bits and classify free, reserved, bad, data, and EOC
  values.
- Follow the selected FAT and reject out-of-range links.
- Bound every traversal by the volume's cluster count to detect cycles.
- Read arbitrary unaligned ranges across sectors and fragmented clusters.
- Clamp reads to EOF and permit zero-length files with cluster zero.

Gate: tests cover contiguous and fragmented chains, offsets at every boundary,
short final clusters, I/O failure, bad/reserved/free links, and cycles.

### F3 — Directories and paths

- Iterate 32-byte short entries across sector and cluster boundaries.
- Respect deleted/end markers and skip volume-label entries.
- Decode 8.3 aliases with ASCII case-insensitive lookup.
- Assemble VFAT long-name entries, validate ordinal order, type, first-cluster
  field, checksum, UTF-16 termination, padding, and surrogate pairs.
- Fall back safely to the short alias when long-name metadata is orphaned or
  malformed.
- Traverse absolute or root-relative paths with repeated separators and nested
  directories.

Gate: synthetic directory tests cover deleted entries, early termination,
multi-entry LFNs crossing a sector boundary, checksum/ordinal corruption,
Unicode, alias lookup, nested paths, and non-directory traversal.

### F4 — Integration and compatibility corpus

- Add a block-device adapter for the storage service.
- Test images made independently by platform formatting tools at all supported
  sector/cluster sizes.
- Differentially compare read-only tree walks and file hashes with platform
  tools; generated images are test data, never source-code inputs.
- Add malformed-image fuzz/property tests under host sanitizers.

Gate: every corpus image has a checked manifest and identical file bytes.

### F5 — Controlled mutation

- Add allocation scanning using FSInfo only as a checked hint.
- Implement file creation/growth/truncation, LFN creation, and directory growth.
- Order writes so data and inactive metadata are durable before links become
  visible; update all mirrored FATs as required.
- Add explicit flush/barrier operations to the device contract.
- Add deterministic power-loss injection at every write boundary and a repair
  policy for interrupted operations.

Gate: remount-after-every-write failure tests preserve the old or new state
without cross-links, lost allocated clusters, or exposing uninitialized data.

## Explicit initial limits

The first implementation is read-only. Locale-specific OEM short-name folding,
write support, repair, access-time updates, and FAT12/FAT16 are later work.
UTF-16 long names are decoded to UTF-8; lookup folds ASCII only and compares
other UTF-8 bytes exactly.

