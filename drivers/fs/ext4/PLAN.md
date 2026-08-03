# ext4 Driver Development Plan

## Contract and sources

The driver is a freestanding C++ component over the repository's
`ReadableDevice` byte-range interface. It uses bounded caller-owned objects,
checked arithmetic, explicit errors, and no exceptions or dynamic allocation.

The format rules come from the Linux kernel project's published
[ext4 on-disk documentation](https://docs.kernel.org/filesystems/ext4/),
including its pages for the
[superblock](https://docs.kernel.org/filesystems/ext4/super.html),
[block groups](https://docs.kernel.org/filesystems/ext4/group_descr.html),
[inodes](https://docs.kernel.org/filesystems/ext4/inodes.html),
[extent trees](https://docs.kernel.org/filesystems/ext4/ifork.html), and
[directories](https://docs.kernel.org/filesystems/ext4/directory.html).
No ext4 implementation source is an input to this work.

## Milestones

### E1 — Validated read-only mount

- Read the 1024-byte superblock at byte offset 1024.
- Decode block size/count, group geometry, inode geometry, descriptor size,
  revision, and compatible/incompatible feature masks.
- Reject unknown incompatible features and arithmetic/media overflows.
- Support 32-byte and 64-byte group descriptors, including high inode-table
  block bits.
- Start with 1/2/4 KiB blocks; add larger blocks only with an explicit
  target-memory policy.

Gate: table-driven tests cover each block size, descriptor layout, unsupported
feature, invalid geometry, truncated device, and read error.

### E2 — Inodes and logical block mapping

- Locate inodes through group descriptors using the specified one-based inode
  numbering formulas.
- Decode type, mode, 64-bit size, flags, generation, and `i_block`.
- Support extent roots and external extent nodes through the maximum declared
  depth, validating magic, capacity, sorted ranges, depth transitions, and
  physical bounds.
- Return zeroes for holes and uninitialized extents.
- Support legacy direct, single-, double-, and triple-indirect maps with
  bounded traversal.

Gate: tests cover multiple groups, inode records crossing device blocks,
inline/external extents, sparse/uninitialized ranges, every legacy map level,
malformed trees, and injected I/O failures.

### E3 — Directories, lookup, and file reads

- Parse classic directory entries per filesystem block.
- Validate record length, alignment, name length, inode range, and block bounds.
- Handle both directory-entry layouts selected by the file-type feature.
- Traverse indexed directories safely by scanning their directory file blocks;
  index nodes appear as unused entries, while leaf blocks remain ordinary
  directory blocks.
- Provide exact byte-name lookup, nested paths, arbitrary partial file reads,
  EOF clamping, and sparse zero filling.

Gate: tests cover deleted entries, checksum tails, indexed-directory interior
blocks, malformed records, nested traversal, holes, fragmented extents, and
non-directory path components.

### E4 — Integrity and broader read compatibility

- Implement CRC32C metadata checksum verification for the superblock, group
  descriptors, inodes, extent nodes, and directory blocks.
- Add fast symlinks and block-backed symlink traversal with loop limits.
- Add inline-data, large-directory, meta-block-group, and bigalloc support as
  separately gated features.
- Test images made independently by `mke2fs` across feature combinations.
- Add malformed-image fuzz/property tests under host sanitizers.

Gate: every enabled feature has positive, checksum-corruption, truncation, and
cross-structure inconsistency tests; unsupported features fail at mount or
operation boundaries, never silently.

### E5 — Journal-aware mutation

- Implement allocation bitmaps and verified free-count updates.
- Add create/write/truncate/link/unlink/rename with orphan handling.
- Implement JBD2 transaction parsing, checksums, replay, commit, revoke, flush,
  and recovery rules before advertising writable mounts.
- Inject power loss at each metadata/data/journal write and remount after every
  cut.

Gate: transactions are atomic under the declared data mode, replay is
idempotent, metadata checksums remain valid, and independent filesystem tools
report a clean filesystem.

## Explicit initial limits

The first implementation is read-only and supports 1/2/4 KiB blocks, classic
directories, extents, and legacy block maps. It rejects meta block groups,
bigalloc, inline data, encryption, and casefolding. Metadata-checksummed images
are accepted read-only in the first slice but reported as not yet integrity
verified; E4 makes verification mandatory before production use.

