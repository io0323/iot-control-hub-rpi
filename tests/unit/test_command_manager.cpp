// CommandManager（基本設計 第6章「指令のライフサイクル」「送信の制限」）を仮想時計で確かめる：
// ライフサイクルの全遷移、2秒の境界と3回の再送、TIMED_OUT、UART の毎秒1件・未完了4件と再送の優先、
// 置き換え（open-questions #21）、expires_at（#40・#43）、offline の即 FAILED、リースの更新
#include <gtest/gtest.h>

#include <set>

#include "core/command/CommandManager.h"
#include "core/common/Clock.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"
#include "core/uart/UartBridge.h"

namespace c = gw::core::command;
namespace u = gw::core::uart;
using gw::core::Json;
using gw::core::Millis;

namespace {

constexpr auto kUart = "mcu-c-01";
constexpr auto kMqtt = "mcu-cpp-01";

class CommandManagerTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-26T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  u::UartBridge bridge{u::load_profile(GW_PROFILES_DIR, kUart), ids, std::string{"gw-01"}};
  std::set<std::string> offline;
  c::CommandManager cm{bridge, clock, ids, kUart,
                       [this](const std::string& id) { return !offline.contains(id); }};

  c::CommandOutputs tick(Millis d) {
    clock.advance(d);
    c::CommandOutputs out;
    cm.on_timer(clock.steady_now(), out);
    return out;
  }
  std::string submit(const std::string& target, Json payload, c::CommandOutputs& out,
                     const std::string& owner = "") {
    return cm.submit(c::CommandManager::Submit{target, std::move(payload), owner, true}, out);
  }
  c::CommandOutputs ack(const std::string& cmd_id, const std::string& status,
                        const Json& reason = nullptr) {
    c::CommandOutputs out;
    cm.on_ack(Json{{"cmd_id", cmd_id}, {"status", status}, {"limited", false}, {"reason", reason}},
              out);
    return out;
  }
  static Json fan(int level, int lease_s = 30) {
    return Json{{"op", "set_actuator"},
                {"args", Json{{"id", "fan_main"}, {"level", level}}},
                {"priority", "manual"},
                {"lease_s", lease_s}};
  }
  std::string ts_after(Millis d) const { return gw::core::format_ts(clock.system_now() + d); }
};

}  // namespace

TEST_F(CommandManagerTest, MqttLifecycleSentAcceptedDone) {
  c::CommandOutputs out;
  const auto id = submit(kMqtt, fan(800), out);
  ASSERT_EQ(out.mqtt.size(), 1U);
  EXPECT_EQ(out.mqtt[0].target, kMqtt);
  EXPECT_EQ(out.mqtt[0].payload["cmd_id"], id);
  EXPECT_EQ(out.mqtt[0].payload.begin().key(), "cmd_id");  // 共通仕様の例と同じ並び
  EXPECT_EQ(cm.find(id)->state, c::CmdState::kSent);

  EXPECT_TRUE(ack(id, "accepted").finished.empty());
  EXPECT_EQ(cm.find(id)->state, c::CmdState::kAccepted);
  // ACCEPTED のあとは再送しない
  EXPECT_TRUE(tick(Millis{10'000}).mqtt.empty());
  const auto done = ack(id, "done");
  ASSERT_EQ(done.finished.size(), 1U);
  EXPECT_EQ(done.finished[0].state, c::CmdState::kDone);
  EXPECT_EQ(cm.find(id), nullptr);
  EXPECT_EQ(cm.stats().done, 1U);
}

TEST_F(CommandManagerTest, ImmediateDoneSkipsAccepted) {
  c::CommandOutputs out;
  const auto id = submit(kMqtt, fan(800), out);
  const auto done = ack(id, "done");
  ASSERT_EQ(done.finished.size(), 1U);
  EXPECT_EQ(done.finished[0].state, c::CmdState::kDone);
}

TEST_F(CommandManagerTest, RejectedAndFailedAcksAreTerminal) {
  c::CommandOutputs out;
  const auto a = submit(kMqtt, fan(800), out);
  const auto b = submit(kMqtt, fan(900), out);
  const auto c1 = submit(kMqtt, fan(100), out);
  const auto r = ack(a, "rejected", "unsafe");
  ASSERT_EQ(r.finished.size(), 1U);
  EXPECT_EQ(r.finished[0].state, c::CmdState::kRejected);
  EXPECT_EQ(r.finished[0].reason, "unsafe");
  (void)ack(b, "accepted");
  const auto f = ack(b, "failed", "internal");
  ASSERT_EQ(f.finished.size(), 1U);
  EXPECT_EQ(f.finished[0].state, c::CmdState::kFailed);
  const auto f2 = ack(c1, "failed");  // reason が無ければ internal
  EXPECT_EQ(f2.finished[0].reason, "internal");
  // 終わった指令への ACK は数えるだけ
  EXPECT_TRUE(ack(a, "done").finished.empty());
  EXPECT_EQ(cm.stats().unknown_ack, 1U);
}

TEST_F(CommandManagerTest, MqttResendsAtTwoSecondBoundaryThreeTimesThenTimesOut) {
  c::CommandOutputs out;
  const auto id = submit(kMqtt, fan(800), out);
  ASSERT_EQ(out.mqtt.size(), 1U);
  int sends = 1;
  for (int retry = 0; retry < 3; ++retry) {
    EXPECT_TRUE(tick(Millis{1999}).mqtt.empty()) << "2秒より前に再送した";
    const auto again = tick(Millis{1});
    ASSERT_EQ(again.mqtt.size(), 1U);
    EXPECT_EQ(again.mqtt[0].payload["cmd_id"], id);  // 同じ cmd_id
    ++sends;
  }
  EXPECT_TRUE(tick(Millis{1999}).finished.empty());
  const auto end = tick(Millis{1});
  ASSERT_EQ(end.finished.size(), 1U);
  EXPECT_EQ(end.finished[0].state, c::CmdState::kTimedOut);
  EXPECT_EQ(end.finished[0].reason, c::kReasonTimeout);
  EXPECT_EQ(end.finished[0].attempts, 4);
  EXPECT_EQ(sends, 4);
  EXPECT_EQ(cm.stats().mqtt_retries, 3U);
  EXPECT_EQ(cm.next_deadline(), std::nullopt);
}

TEST_F(CommandManagerTest, OfflineTargetFailsImmediatelyWithoutSending) {
  offline.insert(kMqtt);
  offline.insert(kUart);
  c::CommandOutputs out;
  (void)submit(kMqtt, fan(800), out);
  (void)submit(kUart, fan(800), out);
  EXPECT_TRUE(out.mqtt.empty());
  EXPECT_TRUE(out.uart.empty());
  ASSERT_EQ(out.finished.size(), 2U);
  EXPECT_EQ(out.finished[0].state, c::CmdState::kFailed);
  EXPECT_EQ(out.finished[0].reason, c::kReasonOffline);
  // 復帰しても古い指令をまとめて送らない
  offline.clear();
  EXPECT_TRUE(tick(Millis{5000}).uart.empty());
}

TEST_F(CommandManagerTest, UartOnePerSecondWithRetriesFirstThenTimeout) {
  c::CommandOutputs out;
  const auto a = submit(kUart, fan(100), out, "o1");
  const auto b = submit(kUart, fan(200), out, "o2");
  const auto c3 = submit(kUart, fan(300), out, "o3");
  ASSERT_EQ(out.uart.size(), 1U);  // 毎秒1件
  EXPECT_EQ(out.uart[0].cmd_ref, 0);
  // 1秒ごとの送信：B、A の再送、B の再送 …（再送を新しい指令より先に）
  const std::vector<std::uint16_t> expected{1, 0, 1, 0, 1, 0, 1};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_TRUE(tick(Millis{999}).uart.empty());
    const auto o = tick(Millis{1});
    ASSERT_EQ(o.uart.size(), 1U) << i;
    EXPECT_EQ(o.uart[0].cmd_ref, expected[i]) << i;
    EXPECT_LE(cm.uart_in_flight(), 4U);
  }
  // 8秒：A は4回送って2秒 → TIMED_OUT。空いた枠で C を送る
  const auto t8 = tick(Millis{1000});
  ASSERT_EQ(t8.finished.size(), 1U);
  EXPECT_EQ(t8.finished[0].cmd_id, a);
  EXPECT_EQ(t8.finished[0].state, c::CmdState::kTimedOut);
  ASSERT_EQ(t8.uart.size(), 1U);
  EXPECT_EQ(t8.uart[0].cmd_ref, 2);
  const auto t9 = tick(Millis{1000});
  ASSERT_EQ(t9.finished.size(), 1U);
  EXPECT_EQ(t9.finished[0].cmd_id, b);
  (void)c3;
}

TEST_F(CommandManagerTest, UartInFlightNeverExceedsFour) {
  c::CommandOutputs out;
  std::vector<std::string> ids_sent;
  for (int i = 0; i < 6; ++i) {
    ids_sent.push_back(submit(kUart, fan(100 + i), out, "o" + std::to_string(i)));
  }
  // ACK を返さないまま進めても、未完了は4件まで・毎秒1件まで
  std::set<std::uint16_t> refs;
  for (const auto& f : out.uart) {
    refs.insert(f.cmd_ref);
  }
  for (int s = 0; s < 30; ++s) {
    const auto o = tick(Millis{1000});
    EXPECT_LE(o.uart.size(), 1U);
    EXPECT_LE(cm.uart_in_flight(), 4U);
    for (const auto& f : o.uart) {
      refs.insert(f.cmd_ref);
    }
  }
  EXPECT_EQ(refs.size(), 6U);  // 全件、いずれは送られて終わる
  EXPECT_EQ(cm.stats().timed_out, 6U);
}

TEST_F(CommandManagerTest, UartAckFreesSlotAndNextCommandGoesOut) {
  c::CommandOutputs out;
  const auto a = submit(kUart, fan(100), out, "o1");
  (void)submit(kUart, fan(200), out, "o2");
  ASSERT_EQ(out.uart.size(), 1U);
  const auto done = ack(a, "done");
  EXPECT_TRUE(done.uart.empty());  // 送信枠（毎秒1件）は ACK では空かない
  const auto t1 = tick(Millis{1000});
  ASSERT_EQ(t1.uart.size(), 1U);
  EXPECT_EQ(t1.uart[0].cmd_ref, 1);
  EXPECT_EQ(cm.uart_in_flight(), 1U);
}

TEST_F(CommandManagerTest, ExpiresAtIsCheckedOnlyBeforeTheFirstSend) {
  {
    c::CommandOutputs out;
    auto p = fan(800);
    p["expires_at"] = ts_after(Millis{-1});
    const auto id = submit(kUart, p, out);
    EXPECT_TRUE(out.uart.empty());  // UART に送らない（#40）
    ASSERT_EQ(out.finished.size(), 1U);
    EXPECT_EQ(out.finished[0].cmd_id, id);
    EXPECT_EQ(out.finished[0].state, c::CmdState::kRejected);
    EXPECT_EQ(out.finished[0].reason, c::kReasonExpired);
  }
  {
    c::CommandOutputs out;
    auto p = fan(800);
    p["expires_at"] = ts_after(Millis{500});
    const auto id = submit(kUart, p, out);
    ASSERT_EQ(out.uart.size(), 1U);
    // 期限を過ぎても再送は送る（#43）
    const auto again = tick(Millis{2000});
    ASSERT_EQ(again.uart.size(), 1U);
    EXPECT_EQ(again.uart[0].cmd_ref, out.uart[0].cmd_ref);
    (void)ack(id, "done");
  }
  EXPECT_EQ(cm.stats().expired, 1U);
}

TEST_F(CommandManagerTest, ExpiresWhileWaitingInTheQueue) {
  c::CommandOutputs out;
  (void)submit(kUart, fan(100), out, "x");  // t=0 に送る
  auto p = fan(800);
  p["expires_at"] = ts_after(Millis{500});
  (void)submit(kUart, p, out, "y");  // 送信枠は t=1。そのときには期限を過ぎている
  ASSERT_EQ(out.uart.size(), 1U);
  const auto t1 = tick(Millis{1000});
  EXPECT_TRUE(t1.uart.empty());
  ASSERT_EQ(t1.finished.size(), 1U);
  EXPECT_EQ(t1.finished[0].reason, c::kReasonExpired);
}

TEST_F(CommandManagerTest, MqttExpiredBeforeSendIsRejected) {
  c::CommandOutputs out;
  auto p = fan(800);
  p["expires_at"] = ts_after(Millis{0});
  (void)submit(kMqtt, p, out);
  EXPECT_TRUE(out.mqtt.empty());
  ASSERT_EQ(out.finished.size(), 1U);
  EXPECT_EQ(out.finished[0].reason, c::kReasonExpired);
}

TEST_F(CommandManagerTest, QueuedCommandIsReplacedOnlyBySameOwner) {
  c::CommandOutputs out;
  (void)submit(kUart, fan(100), out, "request:a");  // すぐ送られる
  const auto old_rule = submit(kUart, fan(200), out, "rule:r");
  const auto other = submit(kUart, fan(300), out, "request:b");
  (void)submit(kUart, fan(400), out, "rule:r");
  ASSERT_EQ(out.replaced.size(), 1U);
  EXPECT_EQ(out.replaced[0].cmd_id, old_rule);
  EXPECT_EQ(cm.uart_queued(), 2U);
  EXPECT_NE(cm.find(other), nullptr);  // 別の所有者（別の request）は置き換えない（#21）
  EXPECT_EQ(cm.find(old_rule), nullptr);
}

TEST_F(CommandManagerTest, UartBridgeRejectionIsTerminal) {
  c::CommandOutputs out;
  Json p{{"op", "set_actuator"},
         {"args", Json{{"id", "led_grow"}, {"level", 1}}},
         {"priority", "auto"}};
  (void)submit(kUart, p, out);
  EXPECT_TRUE(out.uart.empty());
  ASSERT_EQ(out.finished.size(), 1U);
  EXPECT_EQ(out.finished[0].state, c::CmdState::kRejected);
  EXPECT_EQ(out.finished[0].reason, "unknown_target");
  // 送らなかったので枠は使っていない
  (void)submit(kUart, fan(1), out);
  EXPECT_EQ(out.uart.size(), 1U);
}

TEST_F(CommandManagerTest, InternalGetStatusSkipsOnlineCheck) {
  offline.insert(kUart);
  c::CommandOutputs out;
  (void)cm.submit(c::CommandManager::Submit{kUart, Json{{"op", "get_status"}}, "", false}, out);
  ASSERT_EQ(out.uart.size(), 1U);
  EXPECT_EQ(out.uart[0].op, 0x10);
  EXPECT_EQ(out.uart[0].ttl_s, 0);
}

TEST_F(CommandManagerTest, RequeuedFrameGoesOutAtTheNextSlot) {
  c::CommandOutputs out;
  (void)submit(kUart, fan(100), out);
  ASSERT_EQ(out.uart.size(), 1U);
  cm.requeue_uart_frame(out.uart[0]);
  EXPECT_EQ(cm.next_deadline(), clock.steady_now() + Millis{1000});
  const auto t1 = tick(Millis{1000});
  ASSERT_EQ(t1.uart.size(), 1U);
  EXPECT_EQ(t1.uart[0], out.uart[0]);
  EXPECT_EQ(cm.stats().uart_requeued, 1U);
}

TEST_F(CommandManagerTest, DeadlineWaitsForTheSlotWhenARetryIsDue) {
  c::CommandOutputs out;
  (void)submit(kUart, fan(100), out, "a");
  (void)tick(Millis{1000});
  (void)submit(kUart, fan(200), out, "b");  // t=1 に送られる
  // A の再送（t=2）は送信枠（t=2）と同じ。過去の時刻を返して回り続けない
  const auto next = cm.next_deadline();
  ASSERT_TRUE(next);
  EXPECT_GE(*next, clock.steady_now());
}

TEST_F(CommandManagerTest, KeepRenewsEveryThirdOfLeaseWithNewCmdId) {
  c::CommandOutputs out;
  const auto first = cm.keep("rule:cool", kMqtt, fan(800, 30), out);
  ASSERT_EQ(out.mqtt.size(), 1U);
  (void)ack(first, "done");
  EXPECT_TRUE(tick(Millis{9999}).mqtt.empty());
  const auto r1 = tick(Millis{1});
  ASSERT_EQ(r1.mqtt.size(), 1U);  // 30秒の3分の1 = 10秒
  const auto id1 = r1.mqtt[0].payload["cmd_id"].get<std::string>();
  EXPECT_NE(id1, first);
  EXPECT_EQ(r1.mqtt[0].payload["lease_s"], 30);
  (void)ack(id1, "done");
  const auto r2 = tick(Millis{10'000});
  ASSERT_EQ(r2.mqtt.size(), 1U);
  (void)ack(r2.mqtt[0].payload["cmd_id"], "done");
  cm.stop_keeping("rule:cool", kMqtt, "fan_main");
  EXPECT_TRUE(tick(Millis{30'000}).mqtt.empty());
  EXPECT_EQ(cm.stats().renewals, 2U);
}

TEST_F(CommandManagerTest, KeepDefaultsLeaseToSixtySeconds) {
  c::CommandOutputs out;
  auto p = fan(500);
  p.erase("lease_s");
  (void)cm.keep("rule:x", kMqtt, p, out);
  EXPECT_EQ(out.mqtt[0].payload["lease_s"], 60);
  EXPECT_TRUE(tick(Millis{19'999}).mqtt.size() <= 1U);
}

TEST(CmdState, NamesAndTerminal) {
  EXPECT_EQ(c::to_string(c::CmdState::kIssued), "ISSUED");
  EXPECT_EQ(c::to_string(c::CmdState::kSent), "SENT");
  EXPECT_EQ(c::to_string(c::CmdState::kAccepted), "ACCEPTED");
  EXPECT_EQ(c::to_string(c::CmdState::kDone), "DONE");
  EXPECT_EQ(c::to_string(c::CmdState::kRejected), "REJECTED");
  EXPECT_EQ(c::to_string(c::CmdState::kFailed), "FAILED");
  EXPECT_EQ(c::to_string(c::CmdState::kTimedOut), "TIMED_OUT");
  EXPECT_FALSE(c::is_terminal(c::CmdState::kSent));
  EXPECT_FALSE(c::is_terminal(c::CmdState::kAccepted));
  EXPECT_TRUE(c::is_terminal(c::CmdState::kTimedOut));
}
