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

  auto reused = ptys.open_master();
  MIKOS_CHECK(suite, reused.handle.slot == master.handle.slot);
  MIKOS_CHECK(suite, reused.handle.generation != master.handle.generation);
  MIKOS_CHECK(suite, ptys.set_locked(master.handle, false) ==
                         PtyStatus::bad_handle);
  MIKOS_CHECK(suite, ptys.open_master().status == PtyStatus::success);
  MIKOS_CHECK(suite, ptys.open_master().status == PtyStatus::no_space);

  return suite.finish();
}
