#include <mikos/process/pipe.hpp>

#include <support/test.hpp>

int main() {
  using namespace mikos::process_model;
  mikos::test::Suite suite{"kernel/pipe"};
  PipeTable<2, 8, 4> pipes;

  auto created = pipes.create();
  MIKOS_CHECK(suite, created.status == PipeStatus::success);
  const auto handle = created.handle;
  MIKOS_CHECK(suite, pipes.used() == 1);
  MIKOS_CHECK(suite, pipes.references(handle, PipeEnd::read) == 1);
  MIKOS_CHECK(suite, pipes.references(handle, PipeEnd::write) == 1);
  MIKOS_CHECK(suite, !pipes.readable(handle));
  MIKOS_CHECK(suite, pipes.writable(handle));

  mikos::u8 output[16]{};
  const mikos::u8 first[] = {0, 1, 2, 3, 4, 5};
  MIKOS_CHECK(suite, pipes.write(handle, first, 0).status ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.read(handle, output, 0).status ==
                         PipeStatus::success);
  auto written = pipes.write(handle, first, sizeof(first));
  MIKOS_CHECK(suite, written.status == PipeStatus::success);
  MIKOS_CHECK(suite, written.size == sizeof(first));
  auto read = pipes.read(handle, output, 4);
  MIKOS_CHECK(suite, read.status == PipeStatus::success && read.size == 4);
  for (mikos::u32 i = 0; i < 4; ++i) {
    MIKOS_CHECK(suite, output[i] == i);
  }

  // This write wraps the tail. The resulting stream must remain 4,5,6,7,8,9.
  const mikos::u8 second[] = {6, 7, 8, 9};
  MIKOS_CHECK(suite, pipes.write(handle, second, sizeof(second)).size == 4);
  read = pipes.read(handle, output, 6);
  MIKOS_CHECK(suite, read.size == 6);
  for (mikos::u32 i = 0; i < 6; ++i) {
    MIKOS_CHECK(suite, output[i] == i + 4);
  }

  // Atomic-size writes do not partially enter a nearly full pipe.
  MIKOS_CHECK(suite, pipes.write(handle, first, 6).size == 6);
  const mikos::u8 atomic[] = {10, 11, 12};
  written = pipes.write(handle, atomic, sizeof(atomic));
  MIKOS_CHECK(suite, written.status == PipeStatus::would_block);
  MIKOS_CHECK(suite, written.size == 0);
  // Larger-than-atomic writes are allowed to make bounded partial progress.
  const mikos::u8 large[] = {20, 21, 22, 23, 24};
  written = pipes.write(handle, large, sizeof(large));
  MIKOS_CHECK(suite, written.status == PipeStatus::success);
  MIKOS_CHECK(suite, written.size == 2);
  MIKOS_CHECK(suite, !pipes.writable(handle));
  MIKOS_CHECK(suite, pipes.read(handle, output, 8).size == 8);

  // Dup/fork-style endpoint references delay EOF and broken-pipe transitions.
  MIKOS_CHECK(suite, pipes.retain(handle, PipeEnd::write) ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.release(handle, PipeEnd::write) ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.read(handle, output, 1).status ==
                         PipeStatus::would_block);
  MIKOS_CHECK(suite, pipes.release(handle, PipeEnd::write) ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.readable(handle));
  MIKOS_CHECK(suite, pipes.hung_up(handle, PipeEnd::read));
  MIKOS_CHECK(suite, pipes.read(handle, output, 1).status ==
                         PipeStatus::end_of_file);

  // The pipe survives while the read side exists, then its stale generation is
  // rejected after complete close and slot reuse.
  MIKOS_CHECK(suite, pipes.release(handle, PipeEnd::read) ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.used() == 0);
  MIKOS_CHECK(suite, pipes.read(handle, output, 1).status ==
                         PipeStatus::bad_handle);
  const auto replacement = pipes.create();
  MIKOS_CHECK(suite, replacement.status == PipeStatus::success);
  MIKOS_CHECK(suite, replacement.handle.slot == handle.slot);
  MIKOS_CHECK(suite, replacement.handle.generation != handle.generation);

  MIKOS_CHECK(suite, pipes.release(replacement.handle, PipeEnd::read) ==
                         PipeStatus::success);
  MIKOS_CHECK(suite, pipes.write(replacement.handle, first, 1).status ==
                         PipeStatus::broken_pipe);
  MIKOS_CHECK(suite, pipes.hung_up(replacement.handle, PipeEnd::write));

  const auto another = pipes.create();
  MIKOS_CHECK(suite, another.status == PipeStatus::success);
  MIKOS_CHECK(suite, pipes.create().status == PipeStatus::no_space);

  // Dropbear retains a signal pipe and a listener child-status pipe while it
  // allocates stdin, stdout, and stderr pipes for a remote command. Guard the
  // five simultaneously live pipe objects required before command fork.
  PipeTable<5, 8, 4> ssh_pipes;
  PipeHandle ssh_handles[5]{};
  for (auto& ssh_handle : ssh_handles) {
    const auto ssh_pipe = ssh_pipes.create();
    MIKOS_CHECK(suite, ssh_pipe.status == PipeStatus::success);
    ssh_handle = ssh_pipe.handle;
  }
  MIKOS_CHECK(suite, ssh_pipes.used() == 5);
  MIKOS_CHECK(suite, ssh_pipes.create().status == PipeStatus::no_space);
  for (const auto ssh_handle : ssh_handles) {
    MIKOS_CHECK(suite, ssh_pipes.release(ssh_handle, PipeEnd::read) ==
                           PipeStatus::success);
    MIKOS_CHECK(suite, ssh_pipes.release(ssh_handle, PipeEnd::write) ==
                           PipeStatus::success);
  }
  MIKOS_CHECK(suite, ssh_pipes.used() == 0);

  return suite.finish();
}
