#include <mikos/process/model.hpp>

#include <support/test.hpp>

int main() {
  using namespace mikos::process_model;
  mikos::test::Suite suite{"kernel/process_model"};

  ProcessTable<4> table;
  MIKOS_CHECK(suite, table.initialize() == ProcessStatus::success);
  MIKOS_CHECK(suite, table.invariant());
  MIKOS_CHECK(suite, table.used() == 1);
  MIKOS_CHECK(suite, table.address_spaces() == 1);
  MIKOS_CHECK(suite, table.find(1)->process_group == 1);
  MIKOS_CHECK(suite, table.find(1)->session == 1);

  auto first = table.fork(1);
  MIKOS_CHECK(suite, first.status == ProcessStatus::success);
  MIKOS_CHECK(suite, first.parent_result == 2);
  MIKOS_CHECK(suite, first.child_result == 0);
  MIKOS_CHECK(suite, table.find(2)->parent == 1);
  MIKOS_CHECK(suite, table.find(2)->process_group == 1);
  MIKOS_CHECK(suite, table.address_spaces() == 2);
  MIKOS_CHECK(suite, table.next_runnable(1) == 2);

  // Signal state is inherited by value, then becomes process-local.
  MIKOS_CHECK(suite, table.find(1)->signals.queue(10) == SignalStatus::success);
  auto second = table.fork(1);
  MIKOS_CHECK(suite, second.child == 3);
  MIKOS_CHECK(suite, table.find(3)->signals.pending() == SignalState::bit(10));
  MIKOS_CHECK(suite, table.find(1)->signals.next() == 10);
  MIKOS_CHECK(suite, table.find(3)->signals.pending() == SignalState::bit(10));

  // A normal wait parks while children run and is woken by publication of a
  // zombie. WNOHANG observes children without parking.
  auto no_hang = table.wait(1, {WaitKind::pid, 2}, true);
  MIKOS_CHECK(suite, no_hang.status == ProcessStatus::success);
  MIKOS_CHECK(suite, no_hang.pid == 0);
  auto blocked = table.wait(1, {WaitKind::pid, 2});
  MIKOS_CHECK(suite, blocked.status == ProcessStatus::would_block);
  MIKOS_CHECK(suite, table.find(1)->state == ProcessState::waiting);
  MIKOS_CHECK(suite, table.exit(2, 7u << 8) == ProcessStatus::success);
  MIKOS_CHECK(suite, table.find(1)->state == ProcessState::runnable);
  MIKOS_CHECK(suite, table.find(2)->state == ProcessState::zombie);
  MIKOS_CHECK(suite, table.address_spaces() == 2);  // pid 1 and pid 3
  auto reaped = table.wait(1, {WaitKind::pid, 2});
  MIKOS_CHECK(suite, reaped.status == ProcessStatus::success);
  MIKOS_CHECK(suite, reaped.pid == 2);
  MIKOS_CHECK(suite, reaped.status_word == (7u << 8));
  MIKOS_CHECK(suite, table.find(2) == nullptr);
  MIKOS_CHECK(suite,
              table.wait(1, {WaitKind::pid, 2}).status ==
                  ProcessStatus::no_child);

  // Reaped slots are reusable but PIDs are not confused with slot indices.
  auto reused = table.fork(1);
  MIKOS_CHECK(suite, reused.child == 4);
  MIKOS_CHECK(suite, table.find(4) != nullptr);
  MIKOS_CHECK(suite, table.find(2) == nullptr);
  MIKOS_CHECK(suite, table.fork(1).status == ProcessStatus::success);
  MIKOS_CHECK(suite, table.fork(1).status == ProcessStatus::no_space);
  MIKOS_CHECK(suite, table.invariant());

  // A child's wait must never consume a sibling's zombie. BusyBox probes
  // wait4(-1, WNOHANG) in freshly forked children, so ownership is observable
  // even when execution is serialized by the current target adapter.
  ProcessTable<4> sibling_table;
  MIKOS_CHECK(suite,
              sibling_table.initialize() == ProcessStatus::success);
  const auto sibling_a = sibling_table.fork(1);
  const auto sibling_b = sibling_table.fork(1);
  MIKOS_CHECK(suite,
              sibling_table.exit(sibling_a.child, 3u << 8) ==
                  ProcessStatus::success);
  MIKOS_CHECK(suite,
              sibling_table.wait(sibling_b.child, {WaitKind::any}, true)
                      .status == ProcessStatus::no_child);
  const auto sibling_reaped =
      sibling_table.wait(1, {WaitKind::pid, sibling_a.child});
  MIKOS_CHECK(suite, sibling_reaped.status == ProcessStatus::success);
  MIKOS_CHECK(suite, sibling_reaped.pid == sibling_a.child);
  MIKOS_CHECK(suite, sibling_table.invariant());

  // Group/session rules: a child can form its own group before exec; a session
  // leader and an execed child cannot be moved by the parent.
  MIKOS_CHECK(suite,
              table.set_process_group(1, 3, 0) == ProcessStatus::success);
  MIKOS_CHECK(suite, table.find(3)->process_group == 3);
  MIKOS_CHECK(suite,
              table.create_session(3) == ProcessStatus::permission_denied);
  MIKOS_CHECK(suite, table.mark_exec(4) == ProcessStatus::success);
  MIKOS_CHECK(suite, table.set_process_group(1, 4, 0) ==
                         ProcessStatus::permission_denied);

  // A vfork parent remains parked until exec or exit, but the child remains a
  // distinct address-space record in this bounded flat implementation.
  ProcessTable<3> vfork_table;
  MIKOS_CHECK(suite, vfork_table.initialize() == ProcessStatus::success);
  auto vf = vfork_table.fork(1, true);
  MIKOS_CHECK(suite, vf.status == ProcessStatus::success);
  MIKOS_CHECK(suite,
              vfork_table.find(1)->state == ProcessState::vfork_wait);
  MIKOS_CHECK(suite, vfork_table.mark_exec(vf.child) == ProcessStatus::success);
  MIKOS_CHECK(suite, vfork_table.find(1)->state == ProcessState::runnable);

  // Orphans move to init before their parent is reaped.
  auto grandchild = vfork_table.fork(vf.child);
  MIKOS_CHECK(suite, grandchild.status == ProcessStatus::success);
  MIKOS_CHECK(suite, vfork_table.exit(vf.child, 0) == ProcessStatus::success);
  MIKOS_CHECK(suite, vfork_table.find(grandchild.child)->parent == 1);
  MIKOS_CHECK(suite, vfork_table.invariant());

  return suite.finish();
}
