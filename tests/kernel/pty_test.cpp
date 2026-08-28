#include <mikos/process/pty.hpp>

#include <support/test.hpp>

int main() {
  using namespace mikos::process_model;
  mikos::test::Suite suite{"kernel/pty"};
  PtyTable<2, 8> ptys;

  auto master = ptys.open_master();
  MIKOS_CHECK(suite, master.status == PtyStatus::success);
  MIKOS_CHECK(suite, ptys.used() == 1);
  MIKOS_CHECK(suite, ptys.open_slave(master.handle.slot).status ==
                         PtyStatus::locked);
  MIKOS_CHECK(suite, ptys.set_locked(master.handle, false) ==
                         PtyStatus::success);
  auto slave = ptys.open_slave(master.handle.slot);
  MIKOS_CHECK(suite, slave.status == PtyStatus::success);
  MIKOS_CHECK(suite, slave.handle == master.handle);
  MIKOS_CHECK(suite, ptys.active(master.handle.slot));
  MIKOS_CHECK(suite, ptys.mode(master.handle.slot) == 0666);
  MIKOS_CHECK(suite, ptys.set_mode(master.handle.slot, 010622) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.mode(master.handle.slot) == 0622);

  const mikos::u8 input[] = {'a', 'b', 'c', 'd', 'e', 'f'};
  mikos::u8 output[8]{};
  MIKOS_CHECK(suite, ptys.write(master.handle, PtyEnd::master, input,
                                sizeof(input)).size == sizeof(input));
  MIKOS_CHECK(suite, ptys.read(slave.handle, PtyEnd::slave, output, 4).size ==
                         4);
  MIKOS_CHECK(suite, output[0] == 'a' && output[3] == 'd');
  const mikos::u8 wrap[] = {'g', 'h', 'i', 'j', 'k', 'l'};
  MIKOS_CHECK(suite,
              ptys.write(master.handle, PtyEnd::master, wrap, sizeof(wrap))
                      .size == 6);
  MIKOS_CHECK(suite,
              ptys.read(slave.handle, PtyEnd::slave, output, sizeof(output))
                      .size == sizeof(output));
  MIKOS_CHECK(suite, output[0] == 'e' && output[1] == 'f');
  MIKOS_CHECK(suite, output[2] == 'g' && output[7] == 'l');

  const mikos::u8 reply[] = {'O', 'K'};
  MIKOS_CHECK(suite,
              ptys.write(slave.handle, PtyEnd::slave, reply, sizeof(reply))
                      .size == 2);
  MIKOS_CHECK(suite, ptys.readable(master.handle, PtyEnd::master));
  MIKOS_CHECK(suite,
              ptys.read(master.handle, PtyEnd::master, output, 2).size == 2);
  MIKOS_CHECK(suite, output[0] == 'O' && output[1] == 'K');

  TermiosState raw = *ptys.termios(master.handle);
  raw.local_flags = 0;
  MIKOS_CHECK(suite, ptys.set_termios(slave.handle, raw) == PtyStatus::success);
  MIKOS_CHECK(suite, *ptys.termios(master.handle) == raw);
  bool changed = false;
  WindowSize window{40, 132, 640, 480};
  MIKOS_CHECK(suite, ptys.set_window(master.handle, window, &changed) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, changed);
  MIKOS_CHECK(suite, *ptys.window(slave.handle) == window);
  MIKOS_CHECK(suite, ptys.set_window(slave.handle, window, &changed) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, !changed);

  // Only a session leader acquires a controlling slave. Foreground state is
  // shared by both endpoints and can only be changed by that session.
  MIKOS_CHECK(suite, ptys.acquire_controlling_terminal(
                         slave.handle, 7, 7, 8) ==
                         PtyStatus::permission_denied);
  MIKOS_CHECK(suite, ptys.acquire_controlling_terminal(
                         slave.handle, 7, 7, 7) == PtyStatus::success);
  MIKOS_CHECK(suite, ptys.session(master.handle) == 7);
  MIKOS_CHECK(suite, ptys.foreground_group(master.handle) == 7);
  auto controlling = ptys.open_controlling_slave(7);
  MIKOS_CHECK(suite, controlling.status == PtyStatus::success);
  MIKOS_CHECK(suite, controlling.handle == slave.handle);
  MIKOS_CHECK(suite, ptys.open_controlling_slave(8).status ==
                         PtyStatus::bad_handle);
  MIKOS_CHECK(suite, ptys.release(controlling.handle, PtyEnd::slave) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.set_foreground_group(slave.handle, 8, 9) ==
                         PtyStatus::permission_denied);
  MIKOS_CHECK(suite, ptys.set_foreground_group(slave.handle, 7, 9) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.foreground_group(master.handle) == 9);

  MIKOS_CHECK(suite, ptys.retain(slave.handle, PtyEnd::slave) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.release(slave.handle, PtyEnd::slave) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.release(master.handle, PtyEnd::master) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.read(slave.handle, PtyEnd::slave, output, 1).status ==
                         PtyStatus::end_of_file);
  MIKOS_CHECK(suite, ptys.write(slave.handle, PtyEnd::slave, reply, 1).status ==
                         PtyStatus::io_error);
  MIKOS_CHECK(suite, ptys.release(slave.handle, PtyEnd::slave) ==
                         PtyStatus::success);
  MIKOS_CHECK(suite, ptys.used() == 0);
  MIKOS_CHECK(suite, !ptys.active(master.handle.slot));
  MIKOS_CHECK(suite, ptys.mode(master.handle.slot) == 0);
  MIKOS_CHECK(suite, ptys.set_mode(master.handle.slot, 0600) ==
                         PtyStatus::bad_handle);

  auto reused = ptys.open_master();
  MIKOS_CHECK(suite, reused.handle.slot == master.handle.slot);
  MIKOS_CHECK(suite, reused.handle.generation != master.handle.generation);
  MIKOS_CHECK(suite, ptys.mode(reused.handle.slot) == 0666);
  MIKOS_CHECK(suite, ptys.set_locked(master.handle, false) ==
                         PtyStatus::bad_handle);
  MIKOS_CHECK(suite, ptys.open_master().status == PtyStatus::success);
  MIKOS_CHECK(suite, ptys.open_master().status == PtyStatus::no_space);

  // Closing the final slave must not discard bytes already written to the
  // master. A PTY server commonly receives SIGCHLD before it drains the last
  // command output, and Linux permits that buffered output to be read first.
  PtyTable<1, 8> drain_ptys;
  const auto drain_master = drain_ptys.open_master();
  MIKOS_CHECK(suite, drain_ptys.set_locked(drain_master.handle, false) ==
                         PtyStatus::success);
  const auto drain_slave =
      drain_ptys.open_slave(drain_master.handle.slot);
  const mikos::u8 final_output[] = {'d', 'o', 'n', 'e'};
  MIKOS_CHECK(suite,
              drain_ptys.write(drain_slave.handle, PtyEnd::slave,
                               final_output, sizeof(final_output)).size ==
                  sizeof(final_output));
  MIKOS_CHECK(suite,
              drain_ptys.release(drain_slave.handle, PtyEnd::slave) ==
                  PtyStatus::success);
  MIKOS_CHECK(suite,
              drain_ptys.read(drain_master.handle, PtyEnd::master, output,
                              sizeof(output)).size == sizeof(final_output));
  MIKOS_CHECK(suite, output[0] == 'd' && output[3] == 'e');
  MIKOS_CHECK(suite,
              drain_ptys.read(drain_master.handle, PtyEnd::master, output, 1)
                      .status == PtyStatus::end_of_file);

  // Serialized interactive handoff: the shell writes a prompt and blocks on
  // its slave, the server drains the master and supplies a command, then the
  // shell becomes readable and can answer before closing. This is the exact
  // readiness sequence used by the flat fork adapter for an SSH login shell.
  PtyTable<1, 32> interactive_ptys;
  const auto interactive_master = interactive_ptys.open_master();
  MIKOS_CHECK(suite,
              interactive_ptys.set_locked(interactive_master.handle, false) ==
                  PtyStatus::success);
  const auto interactive_slave =
      interactive_ptys.open_slave(interactive_master.handle.slot);
  const mikos::u8 prompt[] = {'/', ' ', '#', ' '};
  MIKOS_CHECK(
      suite,
      interactive_ptys
              .write(interactive_slave.handle, PtyEnd::slave, prompt,
                     sizeof(prompt))
              .size == sizeof(prompt));
  MIKOS_CHECK(suite,
              !interactive_ptys.readable(interactive_slave.handle,
                                         PtyEnd::slave));
  MIKOS_CHECK(suite,
              interactive_ptys.read(interactive_slave.handle, PtyEnd::slave,
                                    output, sizeof(output)).status ==
                  PtyStatus::would_block);
  MIKOS_CHECK(suite,
              interactive_ptys.read(interactive_master.handle, PtyEnd::master,
                                    output, sizeof(output)).size ==
                  sizeof(prompt));
  MIKOS_CHECK(suite, output[0] == '/' && output[3] == ' ');
  const mikos::u8 command[] = {'e', 'x', 'i', 't', '\n'};
  MIKOS_CHECK(
      suite,
      interactive_ptys
              .write(interactive_master.handle, PtyEnd::master, command,
                     sizeof(command))
              .size == sizeof(command));
  MIKOS_CHECK(suite,
              interactive_ptys.readable(interactive_slave.handle,
                                         PtyEnd::slave));
  MIKOS_CHECK(suite,
              interactive_ptys.read(interactive_slave.handle, PtyEnd::slave,
                                    output, sizeof(output)).size ==
                  sizeof(command));
  MIKOS_CHECK(suite, output[0] == 'e' && output[4] == '\n');
  MIKOS_CHECK(suite,
              interactive_ptys.release(interactive_slave.handle,
                                       PtyEnd::slave) == PtyStatus::success);
  MIKOS_CHECK(suite,
              interactive_ptys.read(interactive_master.handle, PtyEnd::master,
                                    output, 1).status ==
                  PtyStatus::end_of_file);

  // A PTY master must apply the slave's ISIG control characters instead of
  // passing them to the foreground program as ordinary bytes. Signal input is
  // consumed, flushes queued input/output unless NOFLSH is set, and uses
  // ECHOCTL's familiar caret notation when requested.
  PtyTable<1, 32> signal_ptys;
  const auto signal_master = signal_ptys.open_master();
  MIKOS_CHECK(suite,
              signal_ptys.set_locked(signal_master.handle, false) ==
                  PtyStatus::success);
  const auto signal_slave =
      signal_ptys.open_slave(signal_master.handle.slot);
  TermiosState signal_termios = *signal_ptys.termios(signal_slave.handle);
  signal_termios.local_flags |= 0x00000200;  // ECHOCTL
  MIKOS_CHECK(suite,
              signal_ptys.set_termios(signal_slave.handle, signal_termios) ==
                  PtyStatus::success);
  const mikos::u8 stale_input[]{'o', 'l', 'd'};
  MIKOS_CHECK(suite,
              signal_ptys
                      .write(signal_master.handle, PtyEnd::master,
                             stale_input, sizeof(stale_input))
                      .size == sizeof(stale_input));
  const mikos::u8 interrupt[]{3};
  const auto interrupted = signal_ptys.write(
      signal_master.handle, PtyEnd::master, interrupt, sizeof(interrupt));
  MIKOS_CHECK(suite, interrupted.status == PtyStatus::success);
  MIKOS_CHECK(suite, interrupted.size == sizeof(interrupt));
  MIKOS_CHECK(suite,
              interrupted.generated_signals ==
                  SignalState::bit(signal_interrupt));
  MIKOS_CHECK(suite,
              signal_ptys.read(signal_slave.handle, PtyEnd::slave, output, 1)
                      .status == PtyStatus::would_block);
  MIKOS_CHECK(suite,
              signal_ptys.read(signal_master.handle, PtyEnd::master, output,
                               sizeof(output))
                      .size == 2);
  MIKOS_CHECK(suite, output[0] == '^' && output[1] == 'C');

  // NOFLSH preserves bytes already waiting for the slave. VQUIT and VSUSP
  // generate their respective process-group signals without entering that
  // input queue.
  signal_termios.local_flags |= 0x00000080;  // NOFLSH
  MIKOS_CHECK(suite,
              signal_ptys.set_termios(signal_slave.handle, signal_termios) ==
                  PtyStatus::success);
  const mikos::u8 preserved[]{'x'};
  MIKOS_CHECK(suite,
              signal_ptys
                      .write(signal_master.handle, PtyEnd::master, preserved,
                             sizeof(preserved))
                      .size == sizeof(preserved));
  MIKOS_CHECK(suite,
              signal_ptys
                      .write(signal_master.handle, PtyEnd::master, interrupt,
                             sizeof(interrupt))
                      .generated_signals ==
                  SignalState::bit(signal_interrupt));
  MIKOS_CHECK(suite,
              signal_ptys.read(signal_slave.handle, PtyEnd::slave, output, 1)
                      .size == 1);
  MIKOS_CHECK(suite, output[0] == 'x');
  const mikos::u8 quit[]{28};
  MIKOS_CHECK(suite,
              signal_ptys
                      .write(signal_master.handle, PtyEnd::master, quit,
                             sizeof(quit))
                      .generated_signals == SignalState::bit(signal_quit));
  const mikos::u8 suspend[]{26};
  MIKOS_CHECK(
      suite,
      signal_ptys
              .write(signal_master.handle, PtyEnd::master, suspend,
                     sizeof(suspend))
              .generated_signals == SignalState::bit(signal_tty_stop));

  // Raw mode and an explicitly disabled control-character slot pass the byte
  // through unchanged and generate no signal.
  TermiosState raw_signal_termios = signal_termios;
  raw_signal_termios.local_flags = 0;
  MIKOS_CHECK(
      suite,
      signal_ptys.set_termios(signal_slave.handle, raw_signal_termios) ==
          PtyStatus::success);
  const auto raw_interrupt = signal_ptys.write(
      signal_master.handle, PtyEnd::master, interrupt, sizeof(interrupt));
  MIKOS_CHECK(suite, raw_interrupt.generated_signals == 0);
  MIKOS_CHECK(suite,
              signal_ptys.read(signal_slave.handle, PtyEnd::slave, output, 1)
                      .size == 1);
  MIKOS_CHECK(suite, output[0] == 3);
  TermiosState disabled_interrupt = signal_termios;
  disabled_interrupt.control[0] = 0;
  MIKOS_CHECK(
      suite,
      signal_ptys.set_termios(signal_slave.handle, disabled_interrupt) ==
          PtyStatus::success);
  const mikos::u8 nul[]{0};
  const auto disabled_result = signal_ptys.write(
      signal_master.handle, PtyEnd::master, nul, sizeof(nul));
  MIKOS_CHECK(suite, disabled_result.generated_signals == 0);
  MIKOS_CHECK(suite,
              signal_ptys.read(signal_slave.handle, PtyEnd::slave, output, 1)
                      .size == 1);
  MIKOS_CHECK(suite, output[0] == 0);

  SignalState pending_signal;
  MIKOS_CHECK(suite, !pending_signal.has_deliverable());
  MIKOS_CHECK(suite, pending_signal.queue(signal_interrupt) ==
                         SignalStatus::success);
  MIKOS_CHECK(suite, pending_signal.has_deliverable());
  MIKOS_CHECK(suite,
              pending_signal.change_mask(
                  SignalMaskOperation::block,
                  SignalState::bit(signal_interrupt)) ==
                  SignalStatus::success);
  MIKOS_CHECK(suite, !pending_signal.has_deliverable());
  MIKOS_CHECK(suite,
              pending_signal.change_mask(
                  SignalMaskOperation::unblock,
                  SignalState::bit(signal_interrupt)) ==
                  SignalStatus::success);
  MIKOS_CHECK(suite, pending_signal.has_deliverable());

  return suite.finish();
}
