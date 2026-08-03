#include <mikos/process/signal.hpp>

#include <support/test.hpp>

int main() {
  using namespace mikos::process_model;
  mikos::test::Suite suite{"kernel/signal"};
  SignalState signals;

  MIKOS_CHECK(suite, !SignalState::valid(0));
  MIKOS_CHECK(suite, SignalState::valid(1));
  MIKOS_CHECK(suite, SignalState::valid(64));
  MIKOS_CHECK(suite, signals.queue(65) == SignalStatus::invalid_signal);
  MIKOS_CHECK(suite,
              signals.set_action(signal_kill, {}) == SignalStatus::uncatchable);
  MIKOS_CHECK(suite,
              signals.set_action(signal_stop, {}) == SignalStatus::uncatchable);

  SignalAction action{0x81234000, 0x10000000, 0x81235000,
                      SignalState::bit(2) | SignalState::bit(signal_kill)};
  SignalAction old{};
  MIKOS_CHECK(suite, signals.set_action(10, action, &old) ==
                         SignalStatus::success);
  MIKOS_CHECK(suite, old == SignalAction{});
  MIKOS_CHECK(suite, signals.action(10).handler == action.handler);
  MIKOS_CHECK(suite, (signals.action(10).mask &
                      SignalState::bit(signal_kill)) == 0);

  mikos::u64 previous = ~mikos::u64{0};
  const auto blocked = SignalState::bit(2) | SignalState::bit(10) |
                       SignalState::bit(signal_kill) |
                       SignalState::bit(signal_stop);
  MIKOS_CHECK(suite,
              signals.change_mask(SignalMaskOperation::block, blocked,
                                  &previous) == SignalStatus::success);
  MIKOS_CHECK(suite, previous == 0);
  MIKOS_CHECK(suite,
              (signals.blocked() & SignalState::bit(signal_kill)) == 0);
  MIKOS_CHECK(suite,
              (signals.blocked() & SignalState::bit(signal_stop)) == 0);

  // Standard signals coalesce while blocked; an unblocked lower signal is
  // delivered first, and unblocking makes the coalesced signal deliverable.
  MIKOS_CHECK(suite, signals.queue(10) == SignalStatus::success);
  MIKOS_CHECK(suite, signals.queue(10) == SignalStatus::success);
  MIKOS_CHECK(suite, signals.queue(3) == SignalStatus::success);
  MIKOS_CHECK(suite, signals.next() == 3);
  MIKOS_CHECK(suite, signals.next() == 0);
  MIKOS_CHECK(suite,
              signals.change_mask(SignalMaskOperation::unblock,
                                  SignalState::bit(10)) ==
                  SignalStatus::success);
  MIKOS_CHECK(suite, signals.next() == 10);
  MIKOS_CHECK(suite, signals.next() == 0);

  MIKOS_CHECK(suite, SignalState::default_for(signal_child) ==
                         SignalDefault::ignore);
  MIKOS_CHECK(suite, SignalState::default_for(signal_window_change) ==
                         SignalDefault::ignore);
  MIKOS_CHECK(suite, SignalState::default_for(signal_continue) ==
                         SignalDefault::resume);
  MIKOS_CHECK(suite, SignalState::default_for(signal_tty_stop) ==
                         SignalDefault::stop);
  MIKOS_CHECK(suite,
              SignalState::default_for(signal_kill) == SignalDefault::terminate);

  MIKOS_CHECK(suite,
              signals.change_mask(SignalMaskOperation::set,
                                  SignalState::bit(12), &previous) ==
                  SignalStatus::success);
  MIKOS_CHECK(suite, (previous & SignalState::bit(2)) != 0);
  MIKOS_CHECK(suite, signals.blocked() == SignalState::bit(12));

  return suite.finish();
}
