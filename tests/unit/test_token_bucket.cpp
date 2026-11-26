// トークンバケット（平均毎秒10件・容量50）を仮想時計で確かめる。
// 契約テスト publish_rate と同じ見方：1秒の窓で50件以下、平均で毎秒10件
#include <gtest/gtest.h>

#include <deque>

#include "core/common/Constants.h"
#include "core/common/TokenBucket.h"

using gw::core::SteadyTime;
using gw::core::TokenBucket;
using std::chrono::milliseconds;
namespace c = gw::core::constants;

TEST(TokenBucket, StartsFullAndRefillsAtRate) {
  SteadyTime now{};
  TokenBucket bucket(c::kPublishRateBurstPerS, c::kPublishRateAvgPerS, now);
  int taken = 0;
  while (bucket.try_take(now)) {
    ++taken;
  }
  EXPECT_EQ(taken, 50);
  // トークンは100ms で1つ貯まるが、直近1秒に50件送っているので1秒たつまで送れない
  EXPECT_EQ(bucket.wait_time(now), std::chrono::nanoseconds{1'000'000'000});
  now += milliseconds{999};
  EXPECT_FALSE(bucket.try_take(now));
  now += milliseconds{1};
  EXPECT_TRUE(bucket.try_take(now));
  now += milliseconds{60'000};
  EXPECT_DOUBLE_EQ(bucket.tokens(now), 50.0);  // 容量で止まる
}

TEST(TokenBucket, GreedySenderStaysWithinBurstAndAverage) {
  SteadyTime now{};
  TokenBucket bucket(c::kPublishRateBurstPerS, c::kPublishRateAvgPerS, now);
  std::deque<SteadyTime> sent;
  int total = 0;
  int worst_window = 0;
  for (int step = 0; step < 10'000; ++step) {  // 10ms ごとに 100 秒
    while (bucket.try_take(now)) {
      sent.push_back(now);
      ++total;
    }
    while (!sent.empty() && now - sent.front() >= milliseconds{1000}) {
      sent.pop_front();
    }
    worst_window = std::max(worst_window, static_cast<int>(sent.size()));
    now += milliseconds{10};
  }
  EXPECT_LE(worst_window, 50);
  EXPECT_LE(total, 50 + 10 * 100);
  EXPECT_GE(total, 10 * 100);
}
