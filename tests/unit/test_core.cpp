// Core（Phase 1）を仮想時計で確かめる：HEARTBEAT の周期、3秒での offline、GET_STATUS の毎秒1件、
// gw-01 の state と LWT
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/Core.h"
#include "core/common/Clock.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"

namespace u = gw::core::uart;
using gw::core::Millis;
using nlohmann::json;

namespace {

class CoreTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-20T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  gw::core::Core core{
      gw::core::CoreConfig{"home", "gw-01", u::load_profile(GW_PROFILES_DIR, "gw-01"),
                           u::load_profile(GW_PROFILES_DIR, "mcu-c-01"), "0.1.0", "0badcafe"},
      clock, ids};

  gw::core::Outputs rx(const u::Frame& frame) {
    return core.step(gw::core::UartReceived{u::Decoded{frame}, clock.system_now()});
  }
  gw::core::Outputs advance(Millis delta) {
    clock.advance(delta);
    return core.on_timer(clock.steady_now());
  }
  static std::vector<u::Cmd> cmds(const gw::core::Outputs& out) {
    std::vector<u::Cmd> result;
    for (const auto& tx : out.uart) {
      if (const auto* c = std::get_if<u::Cmd>(&tx.frame)) {
        result.push_back(*c);
      }
    }
    return result;
  }
  static int heartbeats(const gw::core::Outputs& out) {
    return static_cast<int>(std::count_if(out.uart.begin(), out.uart.end(), [](const auto& tx) {
      return std::holds_alternative<u::Heartbeat>(tx.frame);
    }));
  }
  static std::vector<json> mqtt(const gw::core::Outputs& out) {
    std::vector<json> result;
    for (const auto& tx : out.mqtt) {
      result.push_back(json::parse(tx.payload));
    }
    return result;
  }
};

}  // namespace

TEST_F(CoreTest, HeartbeatEverySecondFromStart) {
  const auto start = core.start();
  EXPECT_EQ(heartbeats(start), 1);
  EXPECT_TRUE(cmds(start).empty());
  EXPECT_EQ(heartbeats(advance(Millis{999})), 0);
  EXPECT_EQ(heartbeats(advance(Millis{1})), 1);
  int total = 0;
  for (int i = 0; i < 10; ++i) {
    total += heartbeats(advance(Millis{1000}));
  }
  EXPECT_EQ(total, 10);
  // 遅れても回を貯めない（1回だけ送り、次は周期に戻る）
  EXPECT_EQ(heartbeats(advance(Millis{3500})), 1);
  EXPECT_EQ(core.next_deadline(), clock.steady_now() + Millis{500});
}

TEST_F(CoreTest, NoGetStatusBeforeAnyReception) {
  (void)core.start();
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(cmds(advance(Millis{1000})).empty());
  }
}

TEST_F(CoreTest, GetStatusOnStateRequestAtMostOncePerSecond) {
  (void)core.start();
  const auto first = rx(u::Telemetry{0, 900, 2500, 0, 0, 0});  // STATE より前
  ASSERT_EQ(cmds(first).size(), 1U);
  EXPECT_EQ(cmds(first)[0].op, 0x10);
  EXPECT_EQ(cmds(first)[0].cmd_ref, 0);
  (void)rx(u::State{0, 0x0102, 1, 1000});
  (void)rx(u::Telemetry{1, 1000, 2500, 0, 1, 0});
  const auto reboot = rx(u::Telemetry{2, 50, 2500, 0, 0, 0});  // uptime の減少
  EXPECT_TRUE(cmds(reboot).empty());  // 前の送信から1秒たっていない
  const auto later = advance(Millis{1000});
  ASSERT_EQ(cmds(later).size(), 1U);
  EXPECT_EQ(cmds(later)[0].cmd_ref, 1);
  // GET_STATUS の ACK は cmd/ack として発行する（open-questions #2）
  const auto ack = rx(u::Ack{0, 1, 1, 0, 0, 0});
  EXPECT_TRUE(ack.mqtt.empty());  // まだ STATE 待ちなので破棄
  (void)rx(u::State{0, 0x0102, 2, 1000});
  const auto ack2 = rx(u::Ack{1, 1, 1, 0, 0, 0});
  ASSERT_EQ(ack2.mqtt.size(), 1U);
  EXPECT_EQ(ack2.mqtt[0].kind, "cmd/ack");
}

TEST_F(CoreTest, OfflineThreeSecondsAfterLastFrameThenOnlineOnState) {
  (void)core.start();
  const auto online = rx(u::State{0, 0x0102, 0x7F3A9C01, 1000});
  ASSERT_EQ(online.mqtt.size(), 1U);
  EXPECT_TRUE(online.mqtt[0].retain);
  EXPECT_TRUE(core.uart_online());
  clock.advance(Millis{1000});
  (void)rx(u::Heartbeat{0, 1000});  // 復号できたフレームなら何でも生存確認を更新する
  auto out = advance(Millis{2999});
  EXPECT_TRUE(out.mqtt.empty());
  out = advance(Millis{1});
  ASSERT_EQ(out.mqtt.size(), 1U);
  const auto offline = json::parse(out.mqtt[0].payload);
  EXPECT_EQ(out.mqtt[0].topic, "iot/v1/home/mcu-c-01/state");
  EXPECT_TRUE(out.mqtt[0].retain);
  EXPECT_EQ(offline["payload"]["status"], "offline");
  EXPECT_EQ(offline["payload"]["bridged_by"], "gw-01");
  EXPECT_EQ(offline["seq"], 1);                     // seq を進めない
  EXPECT_TRUE(advance(Millis{5000}).mqtt.empty());  // 2度は出さない
  const auto back = rx(u::State{1, 0x0102, 0x7F3A9C01, 1000});
  ASSERT_EQ(mqtt(back).size(), 1U);
  EXPECT_EQ(mqtt(back)[0]["payload"]["status"], "online");
  EXPECT_EQ(mqtt(back)[0]["seq"], 1);
}

TEST_F(CoreTest, CorruptFramesDoNotKeepTheLinkAlive) {
  (void)core.start();
  (void)rx(u::State{0, 0x0102, 1, 1000});
  clock.advance(Millis{2000});
  (void)core.step(
      gw::core::UartReceived{u::Decoded{u::Discarded{"crc_mismatch", "", {}}}, clock.system_now()});
  EXPECT_EQ(mqtt(advance(Millis{1000})).size(), 1U);  // 最後の「復号できた」フレームから3秒
}

TEST_F(CoreTest, NoOfflineBeforeFirstStateAndOnLinkDown) {
  (void)core.start();
  (void)rx(u::Heartbeat{0, 100});
  EXPECT_TRUE(advance(Millis{4000}).mqtt.empty());  // boot_id が無いので offline を作れない
  (void)rx(u::State{0, 0x0102, 1, 1000});
  const auto down = core.step(gw::core::UartLinkDown{});
  ASSERT_EQ(mqtt(down).size(), 1U);
  EXPECT_EQ(mqtt(down)[0]["payload"]["status"], "offline");
  EXPECT_TRUE(advance(Millis{4000}).mqtt.empty());
}

TEST_F(CoreTest, GwStateOnEveryConnectAndLwtDoesNotAdvanceSeq) {
  const auto will = core.gw_offline_state();
  EXPECT_EQ(will.topic, "iot/v1/home/gw-01/state");
  EXPECT_EQ(json::parse(will.payload)["seq"], 0);
  const auto first = core.step(gw::core::MqttConnected{});
  ASSERT_EQ(first.mqtt.size(), 1U);
  const auto state = json::parse(first.mqtt[0].payload);
  EXPECT_TRUE(first.mqtt[0].retain);
  EXPECT_EQ(state["payload"]["status"], "online");
  EXPECT_EQ(state["payload"]["mode"], "NORMAL");
  EXPECT_EQ(state["payload"]["fw_version"], "0.1.0");
  EXPECT_EQ(state["boot_id"], "0badcafe");
  EXPECT_EQ(state["ts_source"], "device");
  EXPECT_EQ(state["seq"], 0);
  const auto again = core.step(gw::core::MqttConnected{});
  EXPECT_EQ(json::parse(again.mqtt[0].payload)["seq"], 1);
  const auto bye = core.shutdown();
  ASSERT_EQ(bye.mqtt.size(), 1U);
  EXPECT_EQ(json::parse(bye.mqtt[0].payload)["payload"]["status"], "offline");
  EXPECT_EQ(json::parse(bye.mqtt[0].payload)["seq"], 2);
}

TEST_F(CoreTest, RequeuedCmdIsSentAgainAfterOneSecond) {
  (void)core.start();
  const auto out = rx(u::Telemetry{0, 900, 2500, 0, 0, 0});
  ASSERT_EQ(cmds(out).size(), 1U);
  core.requeue_uart_cmd(cmds(out)[0]);  // uart-out が満杯だった
  const auto later = advance(Millis{1000});
  ASSERT_EQ(cmds(later).size(), 1U);
  EXPECT_EQ(cmds(later)[0], cmds(out)[0]);
}
