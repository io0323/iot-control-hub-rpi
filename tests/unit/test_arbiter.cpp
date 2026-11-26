// Arbiter（基本設計 第6章「調停」）：全優先度の組み合わせ（16通り）、同じ優先度の後勝ち、
// 待ちと引き継ぎ（open-questions #24）、期限切れ、request の release（#26）
#include <gtest/gtest.h>

#include <array>

#include "core/command/Arbiter.h"

namespace c = gw::core::command;
using gw::core::Json;
using gw::core::SteadyTime;
using std::chrono::seconds;

namespace {

const c::ActuatorKey kFan{"mcu-c-01", "fan_main"};
constexpr std::array kAll{c::Priority::kSafety, c::Priority::kManual, c::Priority::kAuto,
                          c::Priority::kSchedule};

c::Claim claim(const std::string& owner, c::Priority p, SteadyTime until, int level = 500) {
  return c::Claim{owner, p, Json{{"id", "fan_main"}, {"level", level}}, Json{}, until, 0};
}

}  // namespace

TEST(Arbiter, AllSixteenPriorityCombinations) {
  const SteadyTime now{};
  int accepted = 0;
  for (const auto held : kAll) {
    for (const auto incoming : kAll) {
      c::Arbiter arbiter;
      ASSERT_TRUE(arbiter.claim(kFan, claim("first", held, now + seconds{60}), now).accepted);
      const auto result = arbiter.claim(kFan, claim("second", incoming, now + seconds{60}), now);
      // 優先度が現所有者以上なら受け入れる（safety ＞ manual ＞ auto ＞ schedule、同じなら後勝ち）
      const bool expected = static_cast<int>(incoming) <= static_cast<int>(held);
      EXPECT_EQ(result.accepted, expected)
          << c::to_string(held) << " が所有、" << c::to_string(incoming) << " が要求";
      EXPECT_EQ(arbiter.owner(kFan)->owner, expected ? "second" : "first");
      if (!expected) {
        EXPECT_EQ(result.owner_priority, held);
        EXPECT_EQ(arbiter.waiting(kFan), 0U);  // 拒否された主張は待ちに入らない
      } else {
        EXPECT_EQ(arbiter.waiting(kFan), 1U);  // 奪われた所有者は期限まで待つ
      }
      accepted += result.accepted ? 1 : 0;
    }
  }
  EXPECT_EQ(accepted, 10);
}

TEST(Arbiter, SamePriorityNewerWins) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  ASSERT_TRUE(
      arbiter.claim(kFan, claim("a", c::Priority::kAuto, now + seconds{60}, 100), now).accepted);
  ASSERT_TRUE(
      arbiter.claim(kFan, claim("b", c::Priority::kAuto, now + seconds{60}, 200), now).accepted);
  EXPECT_EQ(arbiter.owner(kFan)->owner, "b");
  EXPECT_EQ(arbiter.owner(kFan)->args["level"], 200);
}

TEST(Arbiter, SameOwnerReplacesItsOwnClaim) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("rule:x", c::Priority::kAuto, now + seconds{60}, 100), now);
  (void)arbiter.claim(kFan, claim("rule:x", c::Priority::kAuto, now + seconds{60}, 300), now);
  EXPECT_EQ(arbiter.owner(kFan)->args["level"], 300);
  EXPECT_EQ(arbiter.waiting(kFan), 0U);
}

TEST(Arbiter, ReleaseHandsOverToHighestThenNewestWaiting) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("sched", c::Priority::kSchedule, now + seconds{60}), now);
  (void)arbiter.claim(kFan, claim("auto1", c::Priority::kAuto, now + seconds{60}), now);
  (void)arbiter.claim(kFan, claim("auto2", c::Priority::kAuto, now + seconds{60}), now);
  (void)arbiter.claim(kFan, claim("manual", c::Priority::kManual, now + seconds{60}), now);
  EXPECT_EQ(arbiter.waiting(kFan), 3U);

  auto change = arbiter.release(kFan, "manual", now);
  ASSERT_TRUE(change && change->new_owner);
  EXPECT_EQ(change->new_owner->owner, "auto2");  // 同じ優先度では新しい方
  change = arbiter.release(kFan, "auto2", now);
  ASSERT_TRUE(change && change->new_owner);
  EXPECT_EQ(change->new_owner->owner, "auto1");
  change = arbiter.release(kFan, "auto1", now);
  ASSERT_TRUE(change && change->new_owner);
  EXPECT_EQ(change->new_owner->owner, "sched");
  change = arbiter.release(kFan, "sched", now);
  ASSERT_TRUE(change);
  EXPECT_FALSE(change->new_owner);  // 待ちが空なら所有者なし
  EXPECT_EQ(arbiter.owner(kFan), nullptr);
}

TEST(Arbiter, ReleasingAWaitingClaimDoesNotChangeOwner) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("auto", c::Priority::kAuto, now + seconds{60}), now);
  (void)arbiter.claim(kFan, claim("manual", c::Priority::kManual, now + seconds{60}), now);
  EXPECT_FALSE(arbiter.release(kFan, "auto", now));
  EXPECT_EQ(arbiter.waiting(kFan), 0U);
  EXPECT_EQ(arbiter.owner(kFan)->owner, "manual");
  EXPECT_FALSE(arbiter.release(kFan, "nobody", now));
  EXPECT_FALSE(arbiter.release(c::ActuatorKey{"x", "y"}, "manual", now));
}

TEST(Arbiter, ExpiryHandsOverAndSkipsExpiredWaiting) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("auto-short", c::Priority::kAuto, now + seconds{5}), now);
  // 低い優先度は待ちに入らない
  ASSERT_FALSE(
      arbiter.claim(kFan, claim("sched", c::Priority::kSchedule, now + seconds{30}), now).accepted);
  (void)arbiter.claim(kFan, claim("auto-long", c::Priority::kAuto, now + seconds{40}), now);
  (void)arbiter.claim(kFan, claim("manual", c::Priority::kManual, now + seconds{10}), now);
  EXPECT_EQ(arbiter.next_expiry(), now + seconds{5});

  // 5秒：待ちの auto-short が期限切れ（所有者は変わらない）
  EXPECT_TRUE(arbiter.expire(now + seconds{5}).empty());
  EXPECT_EQ(arbiter.waiting(kFan), 1U);
  // 10秒：所有者 manual が期限切れ → auto-long が引き継ぐ
  auto changes = arbiter.expire(now + seconds{10});
  ASSERT_EQ(changes.size(), 1U);
  ASSERT_TRUE(changes[0].new_owner);
  EXPECT_EQ(changes[0].new_owner->owner, "auto-long");
  // 40秒：所有者なし
  changes = arbiter.expire(now + seconds{40});
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_FALSE(changes[0].new_owner);
  EXPECT_EQ(arbiter.next_expiry(), std::nullopt);
}

TEST(Arbiter, ExpiredOwnerDoesNotBlockLowerPriority) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("safety", c::Priority::kSafety, now + seconds{5}), now);
  EXPECT_TRUE(
      arbiter
          .claim(kFan, claim("sched", c::Priority::kSchedule, now + seconds{60}), now + seconds{5})
          .accepted);
}

TEST(Arbiter, ExtendMovesTheLease) {
  c::Arbiter arbiter;
  const SteadyTime now{};
  (void)arbiter.claim(kFan, claim("auto", c::Priority::kAuto, now + seconds{60}), now);
  (void)arbiter.claim(kFan, claim("manual", c::Priority::kManual, now + seconds{60}), now);
  arbiter.extend(kFan, "manual", now + seconds{120});
  arbiter.extend(kFan, "auto", now + seconds{90});
  arbiter.extend(c::ActuatorKey{"x", "y"}, "auto", now);
  EXPECT_TRUE(arbiter.expire(now + seconds{60}).empty());
  EXPECT_EQ(arbiter.next_expiry(), now + seconds{90});
}

TEST(Arbiter, ReleaseForPriority) {
  const SteadyTime now{};
  {
    // 所有者より低い優先度からの解放は何もしない（RequestHandler が lower_priority で拒否する）
    c::Arbiter arbiter;
    (void)arbiter.claim(kFan, claim("safety", c::Priority::kSafety, now + seconds{60}), now);
    EXPECT_FALSE(arbiter.release_for(kFan, c::Priority::kManual, now));
    EXPECT_EQ(arbiter.owner(kFan)->owner, "safety");
  }
  {
    // 手動の解放は auto の所有者も外す（#26）。奪われていた schedule が引き継ぐ
    c::Arbiter arbiter;
    (void)arbiter.claim(kFan, claim("sched", c::Priority::kSchedule, now + seconds{60}), now);
    (void)arbiter.claim(kFan, claim("auto", c::Priority::kAuto, now + seconds{60}), now);
    const auto change = arbiter.release_for(kFan, c::Priority::kManual, now);
    ASSERT_TRUE(change && change->new_owner);
    EXPECT_EQ(change->new_owner->owner, "sched");
  }
  {
    // 所有者が上位でも、同じ優先度の待ちは外れる
    c::Arbiter arbiter;
    (void)arbiter.claim(kFan, claim("manual", c::Priority::kManual, now + seconds{60}), now);
    (void)arbiter.claim(kFan, claim("safety", c::Priority::kSafety, now + seconds{60}), now);
    EXPECT_EQ(arbiter.waiting(kFan), 1U);
    EXPECT_FALSE(arbiter.release_for(kFan, c::Priority::kManual, now));
    EXPECT_EQ(arbiter.waiting(kFan), 0U);
  }
  {
    c::Arbiter arbiter;
    EXPECT_FALSE(arbiter.release_for(kFan, c::Priority::kManual, now));
  }
}
