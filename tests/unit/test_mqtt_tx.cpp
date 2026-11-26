// mqtt-tx（MqttTxWorker）を FakeMqttClient で確かめる：切断中の保持、送信制限、確認待ち32件
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "adapters/mqtt/FakeMqttClient.h"
#include "app/MqttTxWorker.h"
#include "core/common/Clock.h"

using gw::adapters::ConnectionEvent;
using gw::adapters::FakeMqttClient;
using gw::app::MqttTxWorker;
using gw::core::BoundedQueue;
using gw::core::MqttTx;
using namespace std::chrono_literals;

namespace {

MqttTx item(const std::string& kind, const std::string& topic, int n = 0,
            const std::string& device = "mcu-c-01") {
  return MqttTx{topic,
                "{\"n\":" + std::to_string(n) + "}",
                kind == "telemetry" ? 0 : 1,
                kind == "state",
                device,
                kind};
}

class MqttTxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    client.on_connection([&](ConnectionEvent e) { worker.on_connection(e); });
    client.on_published([&](int mid) { worker.on_published(mid); });
  }
  void run_until_drained() {
    queue.close();
    std::thread t([&] { worker.run(); });
    t.join();
  }

  gw::core::SystemClock clock;
  FakeMqttClient client;
  BoundedQueue<MqttTx> queue{4096};
  MqttTxWorker worker{client, queue, clock, 3};
};

}  // namespace

TEST_F(MqttTxTest, HoldsWhileDisconnectedAndFlushesOnReconnect) {
  // 切断中：state はトピックごとの最新1件、event は上限3件、telemetry は捨てる
  ASSERT_TRUE(queue.try_push(item("state", "iot/v1/home/mcu-c-01/state", 1)));
  ASSERT_TRUE(queue.try_push(item("state", "iot/v1/home/mcu-c-01/state", 2)));
  ASSERT_TRUE(queue.try_push(item("telemetry", "iot/v1/home/mcu-c-01/telemetry", 3)));
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue.try_push(item("event", "iot/v1/home/mcu-c-01/event", 10 + i)));
  }
  std::thread t([&] { worker.run(); });
  while (queue.size() > 0) {
    std::this_thread::sleep_for(5ms);
  }
  std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(client.published().empty());
  client.set_auto_ack(true);
  client.set_connected(true);
  ASSERT_TRUE(queue.try_push(item("state", "iot/v1/home/gw-01/state", 99, "gw-01")));
  queue.close();
  t.join();
  const auto published = client.published();
  ASSERT_EQ(published.size(), 5U);  // 保持した state 1件＋event 3件＋新しい state
  EXPECT_EQ(published[0].payload, "{\"n\":2}");
  EXPECT_TRUE(published[0].retain);
  EXPECT_EQ(published[1].payload, "{\"n\":12}");  // 古い event 2件は上限で捨てた
  EXPECT_EQ(published[4].payload, "{\"n\":99}");
  const auto stats = worker.stats();
  EXPECT_EQ(stats.dropped_telemetry_disconnected, 1U);
  EXPECT_EQ(stats.dropped_held, 2U);
}

TEST_F(MqttTxTest, TelemetryOverBurstIsDroppedPerSenderAndKind) {
  client.set_auto_ack(true);
  client.set_connected(true);
  for (int i = 0; i < 60; ++i) {
    ASSERT_TRUE(queue.try_push(item("telemetry", "iot/v1/home/mcu-c-01/telemetry", i)));
  }
  for (int i = 0; i < 5; ++i) {  // 別の発信元は別のバケット
    ASSERT_TRUE(
        queue.try_push(item("telemetry", "iot/v1/home/mcu-cpp-01/telemetry", i, "mcu-cpp-01")));
  }
  run_until_drained();
  const auto stats = worker.stats();
  EXPECT_EQ(stats.published, 55U);  // 1秒の窓で50件まで（発信元ごと）
  EXPECT_EQ(stats.dropped_telemetry_rate, 10U);
}

TEST_F(MqttTxTest, EventsWaitForTokensInsteadOfDropping) {
  client.set_auto_ack(true);
  client.set_connected(true);
  for (int i = 0; i < 52; ++i) {
    ASSERT_TRUE(queue.try_push(item("event", "iot/v1/home/mcu-c-01/event", i)));
  }
  const auto start = std::chrono::steady_clock::now();
  run_until_drained();
  EXPECT_EQ(client.published().size(), 52U);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 900ms);  // 51件目は1秒の窓が空くのを待つ
}

TEST_F(MqttTxTest, AtMostThirtyTwoUnackedQos1Messages) {
  client.set_connected(true);
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(queue.try_push(item("event", "iot/v1/home/mcu-c-01/event", i)));
  }
  queue.close();
  std::thread t([&] { worker.run(); });
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(client.published().size(), 32U);
  EXPECT_EQ(client.unacked().size(), 32U);
  client.ack_all();
  t.join();
  EXPECT_EQ(client.published().size(), 40U);
}
