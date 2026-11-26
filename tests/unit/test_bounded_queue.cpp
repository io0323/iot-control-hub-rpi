#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/common/BoundedQueue.h"

using gw::core::BoundedQueue;
using gw::core::PushResult;
using namespace std::chrono_literals;

TEST(BoundedQueue, TryPushRejectsWhenFull) {
  BoundedQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.try_pop(), 1);
  EXPECT_TRUE(q.try_push(3));
  EXPECT_EQ(q.size(), 2U);
}

TEST(BoundedQueue, EvictsOldestMatchingElement) {
  BoundedQueue<int> q(3);
  ASSERT_TRUE(q.try_push(1));
  ASSERT_TRUE(q.try_push(20));
  ASSERT_TRUE(q.try_push(30));
  const auto even = [](int v) { return v % 2 == 0; };
  EXPECT_EQ(q.try_push_evicting(40, even), PushResult::kEvicted);  // 20 を追い出す
  EXPECT_EQ(q.try_push_evicting(5, [](int) { return false; }), PushResult::kRejected);
  EXPECT_EQ(q.try_pop(), 1);
  EXPECT_EQ(q.try_pop(), 30);
  EXPECT_EQ(q.try_pop(), 40);
  EXPECT_FALSE(q.try_pop().has_value());
}

TEST(BoundedQueue, BlockingPushWaitsForSpace) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    EXPECT_TRUE(q.push(2));
    pushed = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(pushed);
  EXPECT_EQ(q.pop(), 1);
  producer.join();
  EXPECT_TRUE(pushed);
  EXPECT_EQ(q.pop(), 2);
}

TEST(BoundedQueue, PushForTimesOut) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.try_push(1));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.push_for(2, 50ms));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 45ms);
}

TEST(BoundedQueue, EvictingPushBlocksWhenNothingEvictable) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.try_push(1));
  std::thread consumer([&] {
    std::this_thread::sleep_for(30ms);
    (void)q.pop();
  });
  EXPECT_EQ(q.push_evicting(3, [](int v) { return v == 99; }), PushResult::kPushed);
  consumer.join();
  EXPECT_EQ(q.pop(), 3);
}

TEST(BoundedQueue, PopUntilTimesOutAndCloseWakesWaiters) {
  BoundedQueue<int> q(4);
  EXPECT_FALSE(q.pop_until(std::chrono::steady_clock::now() + 20ms).has_value());
  std::thread waiter([&] { EXPECT_FALSE(q.pop().has_value()); });
  std::this_thread::sleep_for(20ms);
  q.close();
  waiter.join();
  EXPECT_FALSE(q.try_push(1));
  EXPECT_TRUE(q.closed());
}

TEST(BoundedQueue, CloseKeepsRemainingItems) {
  BoundedQueue<int> q(4);
  ASSERT_TRUE(q.try_push(1));
  ASSERT_TRUE(q.try_push(2));
  q.close();
  EXPECT_EQ(q.pop(), 1);
  EXPECT_EQ(q.pop(), 2);
  EXPECT_FALSE(q.pop().has_value());
}
