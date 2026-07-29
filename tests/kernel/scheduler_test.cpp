#include <mikos/scheduler.hpp>

#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"kernel/scheduler"};
  mikos::Scheduler scheduler;
  mikos::TrapFrame frame{};
  frame.mepc = 0x11111111;

  scheduler.start_next_on_timer(0x81234000, 0x81fff000);
  scheduler.on_timer(frame, true);
  MIKOS_CHECK(suite, frame.mepc == 0x81234000);
  MIKOS_CHECK(suite, frame.x[2] == 0x81fff000);
  MIKOS_CHECK(suite, scheduler.timer_traps == 1);
  MIKOS_CHECK(suite, scheduler.user_preemptions == 1);
  MIKOS_CHECK(suite, !scheduler.start_pending);

  frame.mstatus = 3u << 11;
  scheduler.on_timer(frame, true);
  MIKOS_CHECK(suite, scheduler.contract_violations == 1);
  MIKOS_CHECK(suite, scheduler.user_preemptions == 1);

  frame.mstatus = 0;
  scheduler.on_timer(frame, false);
  MIKOS_CHECK(suite, scheduler.contract_violations == 2);

  return suite.finish();
}
