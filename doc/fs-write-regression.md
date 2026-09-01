# Writable filesystem regression matrix

This matrix defines the completion criteria for persistent FAT32/ext4 mutation
and the filesystem-independent writable syscall layer. A case is complete only
when it exercises the real on-disk driver, verifies the result after remount,
and checks the exact returned error. In-memory filesystem tests are supplemental
and cannot satisfy a driver case.

## Test levels

- **Unit:** synthetic byte device with exact read/write/flush observation.
- **Remount:** discard the mounted `Volume`, mount the mutated bytes again, and
  verify names, contents, allocation accounting, and metadata.
- **Power cut:** fail before and after every device write and flush; remount must
  expose a valid old or new state and must not expose uninitialized data.
- **Tool:** independently generated image, then `fsck.fat -n` or `e2fsck -fn`.
- **System:** exercise the syscall through BusyBox in QEMU and Tribe, reboot,
  verify contents, and run the host filesystem checker.

## Shared writable-device and VFS cases

- DEV-001 aligned single-sector write/readback.
- DEV-002 aligned multi-sector write/readback.
- DEV-003 unaligned write wholly inside one sector.
- DEV-004 unaligned write spanning two and three sectors.
- DEV-005 zero-length write at start and at device end.
- DEV-006 exact-end write and one-byte out-of-bounds rejection.
- DEV-007 read failure during read-modify-write leaves the sector unchanged.
- DEV-008 write failure is returned and never reported as success.
- DEV-009 flush failure is returned by filesystem sync operations.
- DEV-010 VirtIO write descriptor direction, status, used length, queue wrap,
  device error, timeout, and optional flush-feature negotiation.
- DEV-011 Tribe DMA write command/control sequence, alignment, multi-block
  length, device error, timeout, and fences.
- VFS-001 independent offsets for separately opened descriptions.
- VFS-002 shared offsets after descriptor duplication.
- VFS-003 access-mode enforcement for read-only, write-only, and read-write.
- VFS-004 append writes select EOF atomically for the mounted filesystem.
- VFS-005 descriptor exhaustion, reuse after close, double close, and bad fd.
- VFS-006 directory/file type enforcement and mount-root path handling.
- VFS-007 absolute, cwd-relative, dirfd-relative, `.`, `..`, repeated slash,
  trailing slash, empty path, and maximum-length paths.
- VFS-008 ASCII case folding only on FAT32; byte-exact names on ext4.
- VFS-009 mutation visibility through already-open descriptors and fresh lookup.
- VFS-010 concurrent/preempted mutations serialize without allocator reuse or
  partial directory records.

## FAT32 driver cases

### Writable mount and metadata

- FAT-001 writable mount for every legal sector and cluster size.
- FAT-002 reject FAT12/FAT16, invalid BPB geometry, truncated media, and unknown
  FAT32 version before the first write.
- FAT-003 honor active-FAT selection when mirroring is disabled.
- FAT-004 update every FAT copy when mirroring is enabled.
- FAT-005 preserve the reserved high nibble of each FAT entry.
- FAT-006 accept valid FSInfo counts/hints and ignore invalid signatures,
  out-of-range counts, and allocated hints.
- FAT-007 persist corrected free count/next hint; unknown count remains unknown.
- FAT-008 allocation scans through the end and wraps exactly once.
- FAT-009 full volume returns `no_space` without changing directory metadata.
- FAT-010 bad/reserved/cyclic chains fail without further writes.

### Files and allocation

- FAT-011 create empty and nonempty files; duplicate long name or alias fails.
- FAT-012 write at offset zero, middle, EOF, and beyond EOF.
- FAT-013 writes crossing sector, cluster, and fragmented-chain boundaries.
- FAT-014 growth from zero and growth by multiple noncontiguous clusters.
- FAT-015 gaps and newly allocated clusters read as zero, never old media data.
- FAT-016 overwrite without growth preserves surrounding bytes and allocation.
- FAT-017 truncate within a cluster, at a boundary, across multiple clusters,
  and to zero; freed clusters are reusable.
- FAT-018 append and self-concatenate preserve the original source range.
- FAT-019 maximum 32-bit FAT file size and overflow rejection.
- FAT-020 data is durable before the directory size/chain becomes visible.

### Directories and names

- FAT-021 create all valid 8.3 forms and preserve NT lowercase flags.
- FAT-022 create LFNs of 1, 13, 14, 255 UTF-16 units, including surrogate pairs.
- FAT-023 reject invalid UTF-8, forbidden characters, trailing dot/space,
  reserved dot names, empty components, and overlong names.
- FAT-024 generate collision-free aliases past `~9` and reuse aliases safely.
- FAT-025 directory records crossing sector and cluster boundaries.
- FAT-026 grow a full directory and persist its new cluster chain.
- FAT-027 mkdir initializes `.` and `..` with correct clusters and attributes.
- FAT-028 remove empty files and directories; reject nonempty-directory removal.
- FAT-029 deletion reclaims every associated LFN slot and the data chain.
- FAT-030 same-directory and cross-directory rename, including replacement rules.
- FAT-031 moving a directory updates `..` and rejects moving into its descendant.
- FAT-032 interrupted create/remove/rename leaves no cross-links or visible
  partially initialized LFN sequence.
- FAT-033 timestamps, attributes, volume labels, and end-directory markers remain
  valid after mutation.

### FAT32 validation

- FAT-034 remount after every mutation and compare complete tree/content hashes.
- FAT-035 power cut at every FAT, data, directory, FSInfo, and flush boundary.
- FAT-036 checker-clean independently generated images with one and two FATs.
- FAT-037 allocation cross-link, lost-chain, free-count, and orphan-LFN detector
  tests using intentionally malformed images.

## ext4 driver cases

### Writable mount, features, and recovery

- EXT-001 writable mount for 1/2/4 KiB blocks and 32/64-byte descriptors.
- EXT-002 unknown incompat, bigalloc, encryption, casefold, inline-data, and
  unsupported checksum variants reject writable mount before any write.
- EXT-003 clean no-journal filesystem writes directly with ordered durability.
- EXT-004 needs-recovery filesystem replays JBD2 before normal lookup/mutation.
- EXT-005 journal feature without a valid journal inode/superblock is rejected.
- EXT-006 internal/external journal UUID and block-size validation.
- EXT-007 clean/dirty/error superblock state and mount-count/time transitions.

### Block and inode allocation

- EXT-008 block allocation in first, middle, last partial, and flex block groups.
- EXT-009 block scan skips metadata, wraps groups, and returns `no_space` cleanly.
- EXT-010 block free/reuse, double-free rejection, and reserved-block protection.
- EXT-011 update bitmap, group descriptor, superblock, and high 64-bit free counts.
- EXT-012 inode allocation honors first-nonreserved inode and all group bounds.
- EXT-013 inode free/reuse, double-free rejection, generation update, and zeroing.
- EXT-014 update inode bitmap, free counts, used-directory count, and checksums.
- EXT-015 bitmap/group/inode checksum corruption is detected before mutation.

### Inodes, extents, and file data

- EXT-016 create regular, directory, symlink, and supported special inodes with
  correct mode, uid/gid, links, generation, timestamps, and extra inode size.
- EXT-017 write existing blocks and holes at every block boundary.
- EXT-018 allocate/merge/split extent records and grow from inline extent root.
- EXT-019 grow extent trees through every supported depth and validate indexes.
- EXT-020 convert uninitialized extents only after data is durable.
- EXT-021 legacy direct/single/double/triple-indirect allocation and freeing.
- EXT-022 sparse writes preserve holes; unwritten regions return zero.
- EXT-023 truncate within block, at boundary, through extent nodes, and to zero.
- EXT-024 `i_size`, `i_blocks`, high size, extent generation, and inode checksum.
- EXT-025 append/self-copy, maximum file size, ENOSPC, EFBIG, and overflow.
- EXT-026 fast and block-backed symlink creation, replacement, and truncation.

### Directories, links, and names

- EXT-027 insert using record slack, deleted entries, and newly allocated blocks.
- EXT-028 entries of length 1/255 and records at block boundaries.
- EXT-029 exact case-sensitive duplicate detection and arbitrary valid byte names.
- EXT-030 mkdir writes `.`/`..`, increments parent links and used-dir count.
- EXT-031 unlink decrements links; final close/orphan processing frees the inode.
- EXT-032 hard-link files, reject directory hard links, and enforce link maximum.
- EXT-033 rmdir emptiness, root protection, parent link count, and directory count.
- EXT-034 rename within/cross directory, replacement of compatible types,
  descendant rejection, `..` update, and rename cycle cases.
- EXT-035 indexed-directory insertion/split/hash collision or explicit safe
  conversion/fallback policy.
- EXT-036 directory block tails and checksums remain correct after every change.

### JBD2 atomicity and validation

- EXT-037 descriptor/data/commit transaction parsing for 32/64-bit tags.
- EXT-038 checksum v1/v2/v3 validation and sequence wraparound.
- EXT-039 revoke records suppress stale replay; escape flag restores magic data.
- EXT-040 incomplete/uncommitted transactions are ignored; committed replay is
  idempotent and advances journal/superblock state.
- EXT-041 ordered mode flushes data before metadata commit.
- EXT-042 create/write/truncate/link/unlink/rename atomicity under a power cut at
  every journal and home-block write/flush.
- EXT-043 orphan-list recovery completes interrupted truncate/unlink.
- EXT-044 journal-full wrap, checkpoint, batching, and transaction-size limits.

### ext4 validation

- EXT-045 remount after every mutation and compare tree, metadata, sparse layout,
  link counts, and content hashes.
- EXT-046 `e2fsck -fn` clean after normal tests and every recoverable power cut.
- EXT-047 independently generated feature corpus and malformed metadata corpus.

## Writable syscall cases

- SYS-001 `openat` combinations: `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`,
  `O_DIRECTORY`, `O_NOFOLLOW`, `O_CLOEXEC`, access modes, mode and umask.
- SYS-002 `creat`, `write`, `pwrite64`, `writev`, `pwritev`, partial write,
  zero-length write, invalid user range, and offset overflow.
- SYS-003 `lseek` with SET/CUR/END, sparse seek/write, negative and overflow cases.
- SYS-004 `truncate`, `ftruncate`, and permission/type/error handling.
- SYS-005 `fallocate` supported modes and explicit errors for unsupported modes.
- SYS-006 `mkdirat`, `mknodat`, `symlinkat`, `linkat`, `unlinkat` file/rmdir flags.
- SYS-007 `renameat` and `renameat2` default, NOREPLACE, EXCHANGE, WHITEOUT error.
- SYS-008 `fchmod`, `fchmodat`, `fchown`, `fchownat`, and ownership restrictions.
- SYS-009 `utimensat` NOW/OMIT/null times, symlink flags, and timestamp precision.
- SYS-010 `fsync`, `fdatasync`, `sync`, `syncfs`, and device flush failures.
- SYS-011 writable `mmap` with MAP_SHARED, dirty tracking, `msync`, `munmap`, and
  truncate interaction; MAP_PRIVATE never modifies the file.
- SYS-012 `setxattr`/`lsetxattr`/`fsetxattr` create/replace rules and matching
  removal calls, or explicit unsupported behavior without partial mutation.
- SYS-013 `fcntl` status-flag changes, append behavior, descriptor duplication,
  advisory locks if advertised, and unsupported-command errors.
- SYS-014 stat/fstat/statx/statfs immediately reflect size, blocks, links, modes,
  timestamps, and free-space changes.
- SYS-015 errno mapping for ENOENT, EEXIST, ENOTDIR, EISDIR, ENOTEMPTY, ENOSPC,
  EROFS, ELOOP, ENAMETOOLONG, EINVAL, EBADF, EFAULT, EFBIG, and EIO.
- SYS-016 QEMU and Tribe shell sequence: mkdir, redirection, append, cp, mv, ln,
  symlink, chmod, truncate, rm/rmdir, sync, reboot, verify, and host fsck.
