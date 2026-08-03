# Process, pipe, signal, and terminal implementation plan

## Goal and compatibility boundary

This slice supplies the process substrate needed by an ordinary long-lived
server such as Dropbear: bounded `fork`/`clone`, independently restorable flat
address spaces, inherited descriptors, byte-stream pipes, child notification
and reaping, process groups/sessions, and Unix-98-style PTYs.

`rv32-flat` remains a no-MMU profile.  “Concurrent address spaces” therefore
means that several processes have independent saved register and memory images
and may be runnable at the same time; only the selected image is materialized
in the fixed user virtual range.  A switch at a syscall safe point reloads the
selected executable and restores its writable, anonymous, and live stack
regions.  The timer may request rescheduling but must not copy an image from
interrupt context.  This is functional isolation, not a security or
copy-on-write claim, and process count and image size have explicit bounds.

The slice does not add demand paging, COW, threads sharing an address space,
SMP execution of one process table, real/effective credentials, arbitrary
realtime signals, `signalfd`, packet-mode pipes, PTY packet mode, or a dynamic
kernel allocator.  Those remain separate gates.

## Current landed boundary

The pure bounded process, pipe, signal, and PTY mechanisms and their host tests
are implemented. The live RV32 adapter implements ordinary glibc fork flags,
serial eager parent snapshot/restore, inherited descriptor retention, `pipe2`,
parent-keyed multi-zombie `waitid`, signal action/mask storage, basic
group/session calls, and PTY allocation/I/O/ioctls. BusyBox passes the target
fork/pipe/fd-10/address-restore/nonzero-reap regression across child PIDs 2
through 8.

The live adapter does not yet make the parent and child runnable together.
Consequently nested/background ordering, full-pipe blocking wakeups, signal
handler frames, canonical line discipline, and Dropbear acceptance remain
pending even though their mechanism tables compile and pass host tests.

## Dependency-ordered implementation

1. **Pure bounded mechanisms.** Add separately testable tables for process
   lifecycle, pipe rings, pending signals, process groups/sessions, and PTYs.
   Every capacity and wakeup transition is represented in the type rather than
   inferred by syscall loops.
2. **Per-process resources.** Move registers, cwd/umask, descriptor mappings,
   signal dispositions/mask, parent/group/session IDs, and wait state into a
   process record.  Descriptor inheritance retains shared open descriptions;
   close-on-exec and last-reference teardown are explicit.
3. **Flat address-space switching.** Record executable identity plus disjoint
   writable/anonymous/stack spans.  Validate span order and total size before
   fork.  Save the parent, create an identical child, and reload/restore the
   selected record only at kernel safe points.  Exhaustion returns `EAGAIN` or
   `ENOMEM` without changing either process.
4. **General fork/clone.** Support the asm-generic fork form
   `clone(SIGCHLD, 0, ...)`, retain the existing `CLONE_VM|CLONE_VFORK` launch
   form as bounded vfork-compatible behavior, return zero in the child and its
   PID in the parent, and preserve every register except the ABI result.
5. **Pipes and scheduling.** Implement `pipe2`, `read`, `write`, `close`,
   `fcntl`, `dup3`, `ppoll`, and `pselect6` over bounded byte rings.  Empty/full
   blocking operations park the caller and replay its syscall after a wakeup;
   nonblocking operations return `EAGAIN`.  Closing the last writer produces
   EOF; closing the last reader produces `EPIPE` plus `SIGPIPE`.
6. **Signals and children.** Implement Linux RV32 `rt_sigaction`,
   `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`, pending-mask coalescing,
   default terminate/ignore behavior, user handler frames, `SIGCHLD`, and
   `wait4`/`waitid` with `WNOHANG`.  Zombies retain only reaping information and
   never consume an address-space backing slot.
7. **Groups, sessions, and PTYs.** Implement `setpgid`, `getpgid`, `setsid`,
   `getsid`, `/dev/ptmx`, `/dev/pts/N`, master/slave rings, `grantpt`/`unlockpt`
   ioctls, termios/winsize state, controlling-terminal acquisition, foreground
   group ioctls, hangup, and the job-control signals required by BusyBox and
   Dropbear.
8. **Cross-feature target gates.** Run host mechanism tests, all existing
   regressions, all four RV32 kernel build profiles, QEMU process/pipe tests,
   Tribe interactive shell pipelines and PTY tests, and finally an
   authenticated Dropbear command with stdio attached to a PTY.

## Rational exhaustive regression inventory

“Exhaustive” below means every promised transition, boundary, error class, and
important feature interaction is covered.  Features excluded above are tested
for deterministic rejection rather than accidental partial behavior.

### Process records and general fork

- First process has PID/PGID/SID 1, parent 0, one running record, and no stale
  wait or signal state.
- Fork returns child PID to the parent and zero to the child; both resume after
  the same syscall with all non-result registers preserved.
- Parent and child independently mutate data, BSS, heap, anonymous mappings,
  and live stack, then alternate repeatedly without cross-contamination.
- Text overwritten by a child `execve` is reloaded before the parent resumes;
  two different executables can alternate and exec again.
- Fork preserves cwd, umask, signal actions/mask, process group/session,
  credentials placeholder, and descriptor mappings; subsequent changes to
  process-local state are independent.
- Inherited descriptors refer to the same open description: shared file
  offset/status flags and pipe/socket/PTY endpoint lifetime, but independent
  descriptor flags including close-on-exec.
- Specifically regress BusyBox saving a redirection on fd 10, launching a
  child, and restoring fd 10 afterward; child close/dup must not erase it.
- Parent may run before the child, child before the parent, and siblings in
  round-robin order. Blocking one process must not stop a runnable sibling.
- Child exit before wait, wait before exit, nested children, sibling exit in
  either order, repeated fork/reap slot reuse, and orphan reparenting preserve
  correct PID and status ownership.
- A freshly forked child probing `wait4(-1, WNOHANG)` cannot consume a sibling
  zombie owned by its parent. Later parent-side fork returns must report the
  actual monotonic PID rather than a fixed first-child value.
- Process-table full, PID exhaustion/wrap avoidance, backing-store full,
  malformed spans, oversized heap/mmap/stack, and unsupported clone flags fail
  atomically with the Linux errno selected by the ABI adapter.
- `vfork`-compatible parent suspension ends on child exec or exit. A failed
  exec leaves the child live and the parent suspended. Ordinary fork never
  shares a mutable saved image.
- A zombie owns no runnable context or backing image; only its parent may reap
  it once. PID lookup cannot confuse a reaped record with a reused slot.

### Pipe byte streams

- `pipe2` returns distinct readable/writable descriptors; wrong pointers and
  unsupported flags leave no allocated pipe or descriptor behind.
- Zero-byte read/write succeeds without changing state. One byte, exactly
  capacity, wrap at both ring boundaries, and multiple wrap cycles preserve
  byte order.
- Writes up to `PIPE_BUF` are all-or-nothing when sufficient capacity exists;
  larger writes may make bounded progress but never reorder or invent bytes.
- Empty with writers blocks or returns `EAGAIN`; empty after the last writer is
  EOF. Full with readers blocks or returns `EAGAIN`; no readers returns
  `EPIPE` and queues `SIGPIPE`.
- One writer/many readers and many writers/one reader preserve a valid stream;
  atomic records do not interleave. Wake one/all policy cannot strand a waiter.
- Closing each duplicate changes endpoint counts exactly once. Forked endpoint
  counts, dup replacement, close-on-exec, child exit, and parent exit all wake
  the opposite side at the correct last-reference boundary.
- `poll`/`select` report read data, EOF/HUP, write capacity, and broken-pipe
  error consistently for ordinary, duplicated, inherited, and PTY-backed fds.
- A two-stage and three-stage BusyBox pipeline, early consumer exit (`yes |
  head` shape), bidirectional parent/child control pipes, and pipe-table/fd-table
  exhaustion complete without leaks or deadlock.

### Signals, exit, and reaping

- Validate signal range, reserved/uncatchable signals, sigset size, user
  pointers, and action flags before mutation. Query-only operations are pure.
- Block, unblock, and set-mask return the old mask and never block `SIGKILL` or
  `SIGSTOP`. A blocked standard signal coalesces; unblocking delivers it once.
- Default ignore (`SIGCHLD`), default terminate, explicit ignore, caught
  handler, `SA_RESTART`, `SA_RESETHAND`, `SA_NODEFER`, and handler masks follow
  their documented transitions.
- Signal-frame construction validates aligned writable stack space and
  preserves every register/mask. `rt_sigreturn` restores it exactly; malformed
  or nested-over-capacity frames fail closed.
- `kill` covers positive PID, zero/current group, `-1`, and negative PGID;
  signal zero performs existence/permission probing only. `tgkill` validates
  TGID/TID even though the first profile has one thread per process.
- Signal delivery wakes interruptible pipe/read/wait/poll sleepers and produces
  `EINTR` or syscall restart as selected by the installed action.
- Normal exit, signal death, stopped/continued states, and core-status encoding
  are reported correctly through `wait4` and `waitid`.
- `WNOHANG`, specific PID, any child, process-group selectors, no children,
  non-child PID, bad status/siginfo pointers, repeated wait, and several
  simultaneously waitable children return the correct result/errno.
- Exit queues `SIGCHLD` after publishing zombie state. Ignored/no-cld-wait
  dispositions auto-reap as specified. Reaping cannot race ahead of status
  publication in any tested scheduler ordering.

### Process groups, sessions, and controlling terminals

- `setpgid` covers self/child, new group, existing same-session group, invalid
  group, session leader, child-after-exec, non-child, and vanished PID.
- `setsid` succeeds only for a non-group-leader, makes PID=PGID=SID, and drops
  the controlling terminal. Repeated/leader calls fail without mutation.
- `getpgid`/`getsid` cover self, visible process, absent PID, and post-reap PID.
- A session without a controlling terminal may acquire an eligible PTY slave;
  `TIOCSCTTY` force/non-force, cross-session ownership, non-leader, and repeated
  acquisition cases are deterministic.
- `TIOCSPGRP` accepts only a group in the terminal session; `TIOCGPGRP` reports
  it. Background read/write and terminal-generated interrupt/suspend characters
  target the foreground group with the required job-control signal.
- Session-leader exit hangs up the controlling terminal, sends `SIGHUP` and
  `SIGCONT` to the foreground group, detaches all members, and wakes I/O.

### PTY and termios

- `/dev/ptmx` allocates unique bounded pairs; exhaustion, close, and complete
  close/reopen reuse are leak-free. Slave open is denied until unlock and
  succeeds afterward through the exact `/dev/pts/N` path.
- Master write is slave input and slave write is master input across one byte,
  exact capacity, wrap, partial reads, blocking/nonblocking, poll/select, dup,
  fork, exec, and last-close boundaries.
- Locked slave, wrong PTY number, stale path, master-only/slave-only hangup,
  multiple slave opens, and table exhaustion return stable errors.
- `TIOCGPTN`, `TIOCSPTLCK`, `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`,
  `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGPGRP`, and
  `TIOCSPGRP` validate fd kind and pointers and round-trip state.
- Canonical input editing covers erase, kill, EOF, newline, echo flags, CR/NL
  mappings, output NL/CR mapping, signal characters, disabled control chars,
  and input/output queue capacity. Raw mode transfers bytes unchanged.
- Winsize changes queue `SIGWINCH` to the foreground group only when the value
  changes. Master hangup gives slave read EOF and write `EIO`; final slave close
  gives master read EOF/HUP.
- UART console termios remains usable and distinct from each PTY. Dropbear can
  create a session, assign a controlling slave, dup it to 0/1/2, exec BusyBox
  `sh`, exchange canonical input/output, report a sane winsize, and tear down
  without a zombie or PTY leak.

### Multiway and regression gates

- For each mechanism, run pure state-machine tests, syscall ABI tests with
  invalid user ranges, target single-process tests, target parent-first and
  child-first schedules, and combined shell/server workflows.
- Repeat boundary tests with descriptor aliases, across fork, after exec, after
  signal interruption, and during endpoint/child teardown; these are the
  highest-risk ownership crossings.
- Run table capacities at zero free slots, one free slot, exactly full, and
  after randomized allocate/close/reap reuse. Check invariants after every
  operation with a deterministic seed and an independent reference model.
- Run all existing filesystem, TCP/socket, pseudo-filesystem, scheduler, NIC,
  QEMU, and Tribe tests after each phase. A process feature may not regress the
  already passing remote TCP shell path.
- Final acceptance is an actual Dropbear server listener followed by an
  authenticated host SSH command in a PTY, verified output/exit status, clean
  disconnect, child reap, and a second connection proving resource reuse.

## Completion criteria

The slice is complete only when every implemented row above is automated, all
RV32 profiles build with warnings as errors, QEMU and Tribe target gates pass,
and the Dropbear acceptance test performs a real authenticated command.  Until
then documentation reports each completed sub-gate rather than calling the
system SSH-capable.
