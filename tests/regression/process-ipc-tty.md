# Process, pipe, signal, and PTY regression inventory

The authoritative exhaustive matrix is
[docs/process-ipc-tty-plan.md](../../docs/process-ipc-tty-plan.md).  This file
tracks executable coverage as the phases land.

Current executable gates:

- `tests/kernel/process_model_test.cpp`: bounded process lifecycle, fork result
  semantics, parent/child/group/session validation, sibling-zombie ownership,
  exit, wait selection, reaping, and slot reuse.
- `tests/kernel/pipe_test.cpp`: pipe ring ordering, capacity and wraparound,
  endpoint references, EOF, broken pipe, blocking readiness, and reuse.
- `tests/kernel/signal_test.cpp`: disposition/mask validation, pending-signal
  coalescing, unblock delivery, uncatchable signals, and default actions.
- `tests/kernel/pty_test.cpp`: PTY allocation/locking, bidirectional rings,
  termios and winsize state, foreground groups, hangup, and slot reuse.
- `make tribe-interactive-process-test`: ordinary glibc fork flags, pipe and
  fd-10 inheritance, three-stage BusyBox pipelines, restored parent data, and
  process-group-selected nonzero child reaping through RV32 glibc's `waitid`
  emulation. The sequence reaches PID 8, detecting duplicate parent-side fork
  results as well as stale or cross-parent zombies.
- `tests/tribe/run_interactive_ssh.sh --multicore`: native-C++ Tribe boot,
  background Dropbear listen/accept and none-mode test authentication, nested
  command fork in a second concurrent address space, zombie publication,
  caught `SIGCHLD` delivery through an RV32 Linux-compatible signal frame,
  `rt_sigreturn`, `waitpid` reaping, remote output, exit status, and clean SSH
  channel close.
- All four RV32 kernels compile the live `pipe2`, signal-mask/action,
  process-group/session, `/dev/ptmx`, `/dev/pts/N`, PTY I/O, and terminal-ioctl
  adapters with freestanding warnings treated as errors.

Still pending target gates include general concurrent/background runnable
ordering, blocking pipe wake/replay, canonical line discipline, and a fully
interactive PTY-attached SSH shell.
They must not be inferred from host state-machine coverage or compile success.
