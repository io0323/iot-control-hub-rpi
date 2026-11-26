#include <gtest/gtest.h>

#include <set>

#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/common/Ids.h"

using namespace gw::core;

TEST(Clock, FormatAndParseTs) {
  const auto t = parse_ts("2026-11-20T06:00:12.345Z");
  ASSERT_TRUE(t);
  EXPECT_EQ(format_ts(*t), "2026-11-20T06:00:12.345Z");
  EXPECT_EQ(unix_millis(*t), 1'795'154'412'345);
  EXPECT_EQ(format_ts(SystemTime{}), "1970-01-01T00:00:00.000Z");
  EXPECT_FALSE(parse_ts("2026-11-20T06:00:12Z"));
  EXPECT_FALSE(parse_ts("2026-11-20T06:00:12.345+09:00"));
  EXPECT_FALSE(parse_ts("2026-02-30T06:00:12.345Z"));
}

TEST(Clock, VirtualClockAdvancesBoth) {
  VirtualClock clock(*parse_ts("2026-11-20T06:00:00.000Z"));
  const auto s0 = clock.steady_now();
  clock.advance(Millis{1500});
  EXPECT_EQ(clock.steady_now() - s0, Millis{1500});
  EXPECT_EQ(format_ts(clock.system_now()), "2026-11-20T06:00:01.500Z");
  clock.set_system(*parse_ts("2027-01-01T00:00:00.000Z"));
  EXPECT_EQ(clock.steady_now() - s0, Millis{1500});
}

TEST(Ulid, EncodesLikePythonUlid) {
  // python-ulid：ULID.from_bytes((1795154412345).to_bytes(6,'big') + (1).to_bytes(10,'big'))
  UlidRandom random{};
  random.back() = 1;
  EXPECT_EQ(encode_ulid(1'795'154'412'345, random), "01M7VRN0SS0000000000000001");
  UlidRandom max{};
  max.fill(0xFF);
  EXPECT_EQ(encode_ulid(0xFFFF'FFFF'FFFFULL, max), "7ZZZZZZZZZZZZZZZZZZZZZZZZZ");
}

TEST(Ulid, SequentialMatchesGenGoldenRule) {
  VirtualClock clock(*parse_ts("2026-11-20T06:00:00.500Z"));
  SequentialIdGenerator ids(clock);
  // gen_golden.py の最初の msg_id（10番の次、11番 state_boot の envelope）と同じ規則
  const auto first = ids.next_ulid();
  const auto second = ids.next_ulid();
  EXPECT_EQ(first.size(), 26U);
  EXPECT_EQ(first.substr(0, 10), second.substr(0, 10));
  EXPECT_EQ(first.back(), '1');
  EXPECT_EQ(second.back(), '2');
}

TEST(Ulid, MonotonicWithinSameMillisecond) {
  VirtualClock clock(*parse_ts("2026-11-20T06:00:00.000Z"));
  MonotonicUlidGenerator ids(clock);
  std::set<std::string> seen;
  std::string last;
  for (int i = 0; i < 1000; ++i) {
    auto id = ids.next_ulid();
    EXPECT_GT(id, last);
    last = id;
    seen.insert(std::move(id));
  }
  EXPECT_EQ(seen.size(), 1000U);
  clock.advance(Millis{1});
  EXPECT_GT(ids.next_ulid(), last);
}

TEST(BootId, IsEightLowercaseHex) {
  EXPECT_EQ(format_boot_id(0x7F3A9C01), "7f3a9c01");
  EXPECT_EQ(format_boot_id(1), "00000001");
  EXPECT_EQ(format_boot_id(random_boot_id()).size(), 8U);
}

TEST(SeqCounter, CountsPerKindAndPeekDoesNotAdvance) {
  SeqCounter seq;
  EXPECT_EQ(seq.peek("state"), 0U);
  EXPECT_EQ(seq.next("state"), 0U);
  EXPECT_EQ(seq.next("state"), 1U);
  EXPECT_EQ(seq.next("event"), 0U);
  EXPECT_EQ(seq.peek("state"), 2U);
  EXPECT_EQ(seq.peek("state"), 2U);
}
