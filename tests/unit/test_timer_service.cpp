#include <gtest/gtest.h>

#include "core/common/TimerService.h"

using gw::core::SteadyTime;
using gw::core::TimerKey;
using gw::core::TimerKind;
using gw::core::TimerService;
using std::chrono::milliseconds;

namespace {
const SteadyTime kT0{};
const TimerKey kA{TimerKind::kUartHeartbeat};
const TimerKey kB{TimerKind::kUartLiveness};
const TimerKey kC{TimerKind::kUartCmdSlot};
}  // namespace

TEST(TimerService, FiresInDeadlineOrder) {
  TimerService timers;
  timers.schedule(kT0 + milliseconds{300}, kA);
  timers.schedule(kT0 + milliseconds{100}, kB);
  timers.schedule(kT0 + milliseconds{200}, kC);
  EXPECT_EQ(timers.next_deadline(), kT0 + milliseconds{100});
  EXPECT_TRUE(timers.pop_due(kT0 + milliseconds{50}).empty());
  const auto due = timers.pop_due(kT0 + milliseconds{250});
  ASSERT_EQ(due.size(), 2U);
  EXPECT_EQ(due[0], kB);
  EXPECT_EQ(due[1], kC);
  EXPECT_EQ(timers.next_deadline(), kT0 + milliseconds{300});
}

TEST(TimerService, CancelAndRescheduleInvalidateOldEntries) {
  TimerService timers;
  timers.schedule(kT0 + milliseconds{100}, kA);
  timers.schedule(kT0 + milliseconds{500}, kA);  // 置き換え
  timers.schedule(kT0 + milliseconds{200}, kB);
  timers.cancel(kB);
  EXPECT_FALSE(timers.scheduled(kB));
  EXPECT_EQ(timers.next_deadline(), kT0 + milliseconds{500});
  EXPECT_TRUE(timers.pop_due(kT0 + milliseconds{400}).empty());
  const auto due = timers.pop_due(kT0 + milliseconds{500});
  ASSERT_EQ(due.size(), 1U);
  EXPECT_EQ(due[0], kA);
  EXPECT_FALSE(timers.next_deadline().has_value());
}

TEST(TimerService, SameDeadlineKeepsScheduleOrder) {
  TimerService timers;
  timers.schedule(kT0, kC);
  timers.schedule(kT0, kA);
  const auto due = timers.pop_due(kT0);
  ASSERT_EQ(due.size(), 2U);
  EXPECT_EQ(due[0], kC);
  EXPECT_EQ(due[1], kA);
}
