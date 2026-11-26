// DeviceRegistry と LivenessMonitor（open-questions #4 の各遷移）
#include <gtest/gtest.h>

#include "core/device/DeviceRegistry.h"
#include "core/device/LivenessMonitor.h"
#include "core/uart/Profile.h"

namespace d = gw::core::device;
using d::DeviceStatus;
using gw::core::Json;
using std::chrono::milliseconds;

namespace {

const gw::core::SteadyTime kT0{};

Json state(const std::string& status, std::optional<std::string> bridged_by = std::nullopt) {
  Json p = Json::parse(R"({"status":"online","mode":"auto","fw_version":"1.0",
    "channels":[{"channel":"temp_air","unit":"degC","period_ms":1000},
                {"channel":"soil","unit":"pct","period_ms":10000}],
    "actuators":[{"id":"pump","min":0,"max":1000,"max_on_s":30}]})");
  p["status"] = status;
  if (bridged_by) {
    p["bridged_by"] = *bridged_by;
  }
  return p;
}

}  // namespace

TEST(DeviceRegistry, StaticAndAutoRegistration) {
  d::DeviceRegistry reg;
  reg.register_static(gw::core::uart::load_profile(GW_PROFILES_DIR, "mcu-c-01"));
  const auto* mcu = reg.find("mcu-c-01");
  ASSERT_NE(mcu, nullptr);
  EXPECT_TRUE(mcu->is_static);
  EXPECT_EQ(mcu->transport, "uart");
  EXPECT_FALSE(mcu->status.has_value());  // まだ観測していない
  EXPECT_TRUE(reg.apply_state("mcu-cpp-01", "0000000a", state("online")));
  EXPECT_FALSE(reg.apply_state("mcu-cpp-01", "0000000a", state("online")));
  const auto* cpp = reg.find("mcu-cpp-01");
  ASSERT_NE(cpp, nullptr);
  EXPECT_EQ(cpp->transport, "mqtt");
  EXPECT_EQ(cpp->channels.size(), 2U);
  EXPECT_EQ(cpp->channels.at("soil").period_ms, 10000);
  EXPECT_EQ(cpp->actuators.size(), 1U);
  EXPECT_EQ(cpp->boot_id, "0000000a");
  EXPECT_TRUE(reg.update_reading("mcu-cpp-01", "temp_air", 22.5, "good", kT0));
  EXPECT_FALSE(reg.update_reading("mcu-cpp-01", "lux", 1.0, "good", kT0));
  EXPECT_FALSE(reg.update_reading("nobody", "temp_air", 1.0, "good", kT0));
  EXPECT_EQ(reg.find("mcu-cpp-01")->channels.at("temp_air").value, 22.5);
  // offline の state では channels を消さない
  (void)reg.apply_state("mcu-cpp-01", "0000000a", Json{{"status", "offline"}});
  EXPECT_EQ(reg.find("mcu-cpp-01")->channels.size(), 2U);
}

TEST(DeviceRegistry, BridgedChildIsUnknownWhileBridgeIsOffline) {
  d::DeviceRegistry reg;
  d::LivenessMonitor live(reg);
  (void)reg.apply_state("gw-02", "00000001", state("online"));
  (void)reg.apply_state("mcu-c-02", "00000002", state("online", "gw-02"));
  (void)live.set_status("gw-02", DeviceStatus::kOnline);
  (void)live.set_status("mcu-c-02", DeviceStatus::kOnline);
  EXPECT_EQ(reg.effective_status("mcu-c-02"), DeviceStatus::kOnline);
  const auto change = live.set_status("gw-02", DeviceStatus::kOffline);
  ASSERT_TRUE(change);
  EXPECT_EQ(change->event_code, "DEVICE_OFFLINE");
  EXPECT_EQ(reg.effective_status("mcu-c-02"), DeviceStatus::kUnknown);  // イベントは出さない
  EXPECT_EQ(reg.find("mcu-c-02")->status, DeviceStatus::kOnline);
  (void)live.set_status("gw-02", DeviceStatus::kOnline);
  EXPECT_EQ(reg.effective_status("mcu-c-02"), DeviceStatus::kOnline);
  EXPECT_FALSE(reg.effective_status("nobody").has_value());
}

TEST(LivenessMonitor, EventsOnlyOnOnlineOfflineChangesAndNotOnFirstOnline) {
  d::DeviceRegistry reg;
  d::LivenessMonitor live(reg);
  (void)reg.apply_state("x", "00000001", state("online"));
  auto c = live.set_status("x", DeviceStatus::kOnline);
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->event_code.empty());  // 最初の online はイベントなし
  EXPECT_FALSE(live.set_status("x", DeviceStatus::kOnline));  // 変わらない
  c = live.set_status("x", DeviceStatus::kSuspect);
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->event_code.empty());  // suspect はイベントなし
  c = live.set_status("x", DeviceStatus::kOffline);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->event_code, "DEVICE_OFFLINE");  // suspect → offline も切断の確定
  c = live.set_status("x", DeviceStatus::kOnline);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->event_code, "DEVICE_ONLINE");
  // 最初に観測したのが offline（retain の LWT）なら、次の online で DEVICE_ONLINE
  (void)reg.apply_state("y", "00000001", state("offline"));
  c = live.set_status("y", DeviceStatus::kOffline);
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->event_code.empty());
  EXPECT_EQ(live.set_status("y", DeviceStatus::kOnline)->event_code, "DEVICE_ONLINE");
  EXPECT_FALSE(live.set_status("nobody", DeviceStatus::kOnline));
}

TEST(LivenessMonitor, UartSuspectAtTwoSecondsOfflineAtThree) {
  d::DeviceRegistry reg;
  d::LivenessMonitor live(reg);
  reg.register_static(gw::core::uart::load_profile(GW_PROFILES_DIR, "mcu-c-01"));
  live.uart_frame("mcu-c-01", kT0);
  EXPECT_TRUE(live.uart_check(kT0 + milliseconds{5000}).empty());  // まだ観測していない（STATE 前）
  EXPECT_FALSE(live.uart_next_deadline().has_value());
  (void)live.set_status("mcu-c-01", DeviceStatus::kOnline);
  live.uart_frame("mcu-c-01", kT0);
  EXPECT_EQ(live.uart_next_deadline(), kT0 + milliseconds{2000});
  EXPECT_TRUE(live.uart_check(kT0 + milliseconds{1999}).empty());
  auto changes = live.uart_check(kT0 + milliseconds{2000});
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes[0].to, DeviceStatus::kSuspect);
  EXPECT_EQ(live.uart_next_deadline(), kT0 + milliseconds{3000});
  changes = live.uart_check(kT0 + milliseconds{3000});
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes[0].to, DeviceStatus::kOffline);
  EXPECT_EQ(changes[0].event_code, "DEVICE_OFFLINE");
  EXPECT_FALSE(live.uart_next_deadline().has_value());
  // suspect の間にフレームが戻れば online
  (void)live.set_status("mcu-c-01", DeviceStatus::kOnline);
  live.uart_frame("mcu-c-01", kT0 + milliseconds{10000});
  (void)live.uart_check(kT0 + milliseconds{12500});
  EXPECT_EQ(reg.find("mcu-c-01")->status, DeviceStatus::kSuspect);
  live.uart_frame("mcu-c-01", kT0 + milliseconds{12600});
  EXPECT_EQ(reg.find("mcu-c-01")->status, DeviceStatus::kOnline);
}

TEST(LivenessMonitor, MqttSuspectWhenEveryChannelIsStaleAndBackOnFreshValue) {
  d::DeviceRegistry reg;
  d::LivenessMonitor live(reg);
  (void)reg.apply_state("x", "00000001", state("online"));
  (void)live.set_status("x", DeviceStatus::kOnline);
  EXPECT_TRUE(
      live.stale_check(kT0 + milliseconds{60000}).empty());  // 一度も値が無いチャネルは数えない
  (void)reg.update_reading("x", "temp_air", 1.0, "good", kT0);
  (void)reg.update_reading("x", "soil", 1.0, "good", kT0);
  EXPECT_TRUE(live.stale_check(kT0 + milliseconds{3000}).empty());  // ちょうど3倍は stale でない
  EXPECT_TRUE(live.stale_check(kT0 + milliseconds{3001}).empty());  // temp_air だけ stale
  EXPECT_TRUE(reg.find("x")->channels.at("temp_air").stale);
  EXPECT_FALSE(reg.find("x")->channels.at("soil").stale);
  const auto changes = live.stale_check(kT0 + milliseconds{30001});
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes[0].to, DeviceStatus::kSuspect);
  EXPECT_TRUE(changes[0].event_code.empty());
  (void)reg.update_reading("x", "soil", 2.0, "good", kT0 + milliseconds{31000});
  const auto back = live.fresh_value("x");
  ASSERT_TRUE(back);
  EXPECT_EQ(back->to, DeviceStatus::kOnline);
  EXPECT_FALSE(live.fresh_value("x"));
  EXPECT_EQ(d::to_string(DeviceStatus::kUnknown), "unknown");
}
