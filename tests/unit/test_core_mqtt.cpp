// Core の MQTT 受信（Phase 2）：重複・欠番・時刻ずれ（CLOCK_SKEW は1時間1回）・遅延到着・自動登録・
// stale による suspect・LWT による offline と DEVICE_OFFLINE
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/Core.h"
#include "core/common/Clock.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"

namespace u = gw::core::uart;
using gw::core::Json;
using gw::core::Millis;
using gw::core::device::DeviceStatus;

namespace {

class CoreMqttTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-26T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  gw::core::Core core{gw::core::CoreConfig{"home",
                                           "gw-01",
                                           u::load_profile(GW_PROFILES_DIR, "gw-01"),
                                           u::load_profile(GW_PROFILES_DIR, "mcu-c-01"),
                                           "0.1.0",
                                           "0badcafe",
                                           {},
                                           {}},
                      clock, ids};
  std::uint32_t seq_ = 0;

  void SetUp() override { (void)core.start(); }

  Json envelope(const std::string& schema, std::uint32_t seq, Json payload,
                const std::string& boot = "0000000a",
                const std::optional<std::string>& ts = std::nullopt) {
    return Json{
        {"schema", schema},          {"msg_id", "01JD3R0A5K7M2N4P6R8T0V2X4Y"},
        {"device_id", "mcu-cpp-01"}, {"seq", seq},
        {"boot_id", boot},           {"ts", ts.value_or(gw::core::format_ts(clock.system_now()))},
        {"ts_source", "device"},     {"payload", std::move(payload)}};
  }
  gw::core::Outputs mqtt(const Json& message, const std::string& kind) {
    return core.step(gw::core::MqttReceived{"t", message, kind, clock.system_now(), false});
  }
  gw::core::Outputs online_state(std::uint32_t seq = 0) {
    return mqtt(envelope("iot.v1.state", seq,
                         Json::parse(R"({"status":"online","mode":"auto","fw_version":"1.0",
      "channels":[{"channel":"temp_air","unit":"degC","period_ms":1000}],"actuators":[]})")),
                "state");
  }
  gw::core::Outputs telemetry(std::uint32_t seq, double value,
                              const std::optional<std::string>& ts = std::nullopt) {
    return mqtt(envelope("iot.v1.telemetry", seq,
                         Json{{"readings", Json::array({Json{{"channel", "temp_air"},
                                                             {"value", value},
                                                             {"unit", "degC"},
                                                             {"quality", "good"}}})}},
                         "0000000a", ts),
                "telemetry");
  }
  gw::core::Outputs advance(Millis d) {
    clock.advance(d);
    return core.on_timer(clock.steady_now());
  }
  static std::vector<std::string> codes(const gw::core::Outputs& out) {
    std::vector<std::string> result;
    for (const auto& m : out.mqtt) {
      const auto j = Json::parse(m.payload);
      if (j["schema"] == "iot.v1.event") {
        result.push_back(j["payload"]["code"]);
      }
    }
    return result;
  }
};

}  // namespace

TEST_F(CoreMqttTest, StateRegistersDeviceAndTelemetryUpdatesLatestValue) {
  const auto out = online_state();
  ASSERT_EQ(out.logs.size(), 2U);
  EXPECT_NE(out.logs[0].find("event=device_registered device=mcu-cpp-01"), std::string::npos);
  EXPECT_EQ(core.registry().effective_status("mcu-cpp-01"), DeviceStatus::kOnline);
  (void)telemetry(0, 21.5);
  EXPECT_EQ(core.registry().find("mcu-cpp-01")->channels.at("temp_air").value, 21.5);
  EXPECT_EQ(core.stats().devices_registered, 1U);
}

TEST_F(CoreMqttTest, TelemetryBeforeStateIsNotKept) {
  (void)telemetry(0, 1.0);
  EXPECT_EQ(core.stats().mqtt_unregistered, 1U);
  EXPECT_EQ(core.registry().find("mcu-cpp-01"), nullptr);
}

TEST_F(CoreMqttTest, DuplicatesAreDroppedAndGapsCounted) {
  (void)online_state();
  (void)telemetry(0, 1.0);
  (void)telemetry(1, 2.0);
  (void)telemetry(1, 99.0);  // 重複：最新値を変えない
  EXPECT_EQ(core.registry().find("mcu-cpp-01")->channels.at("temp_air").value, 2.0);
  (void)telemetry(5, 3.0);  // 2・3・4 が欠けた
  EXPECT_EQ(core.stats().mqtt_duplicates, 1U);
  EXPECT_EQ(core.stats().mqtt_gaps, 3U);
  // offline の state は seq の判定から外す（同じ seq でも受け入れる）
  const auto lwt = envelope("iot.v1.state", 0,
                            Json{{"status", "offline"},
                                 {"mode", "auto"},
                                 {"channels", Json::array()},
                                 {"actuators", Json::array()},
                                 {"fw_version", "1.0"}});
  (void)mqtt(lwt, "state");
  (void)mqtt(lwt, "state");
  EXPECT_EQ(core.stats().mqtt_duplicates, 1U);
  EXPECT_EQ(core.registry().effective_status("mcu-cpp-01"), DeviceStatus::kOffline);
}

TEST_F(CoreMqttTest, ClockSkewReplacesTsAndEventIsOncePerHour) {
  (void)online_state();
  const auto skewed = gw::core::format_ts(clock.system_now() - std::chrono::minutes{10});
  auto out = telemetry(0, 1.0, skewed);
  ASSERT_EQ(codes(out), std::vector<std::string>{"CLOCK_SKEW"});
  const auto event = Json::parse(out.mqtt[0].payload);
  EXPECT_EQ(event["device_id"], "gw-01");
  EXPECT_EQ(event["payload"]["severity"], "warning");
  EXPECT_EQ(event["payload"]["detail"]["device_id"], "mcu-cpp-01");
  // ts を受信時刻に置き換えるので、遅延到着とは判定せず最新値を更新する
  EXPECT_EQ(core.registry().find("mcu-cpp-01")->channels.at("temp_air").value, 1.0);
  clock.advance(std::chrono::minutes{59});
  EXPECT_TRUE(
      codes(telemetry(1, 2.0, gw::core::format_ts(clock.system_now() - std::chrono::minutes{10})))
          .empty());
  clock.advance(std::chrono::minutes{1});
  EXPECT_EQ(
      codes(telemetry(2, 3.0, gw::core::format_ts(clock.system_now() - std::chrono::minutes{10}))),
      std::vector<std::string>{"CLOCK_SKEW"});
  EXPECT_EQ(core.stats().mqtt_clock_skew, 3U);
}

TEST_F(CoreMqttTest, LateArrivalDoesNotUpdateLatestValue) {
  (void)online_state();
  (void)telemetry(0, 1.0);
  (void)telemetry(1, 2.0, gw::core::format_ts(clock.system_now() - Millis{3001}));  // 周期の3倍超
  EXPECT_EQ(core.stats().mqtt_late, 1U);
  EXPECT_EQ(core.registry().find("mcu-cpp-01")->channels.at("temp_air").value, 1.0);
}

TEST_F(CoreMqttTest, StaleMakesSuspectThenLwtMakesOfflineWithEvent) {
  (void)online_state();
  (void)telemetry(0, 1.0);
  EXPECT_TRUE(codes(advance(Millis{3000})).empty());
  auto out = advance(Millis{1000});  // 3秒を超えた（1秒周期の判定）
  EXPECT_TRUE(codes(out).empty());   // suspect ではイベントを出さない
  EXPECT_EQ(core.registry().effective_status("mcu-cpp-01"), DeviceStatus::kSuspect);
  EXPECT_EQ(core.stats().stale_transitions, 1U);
  (void)telemetry(1, 2.0);  // 新しい値で online に戻る
  EXPECT_EQ(core.registry().effective_status("mcu-cpp-01"), DeviceStatus::kOnline);
  out = mqtt(envelope("iot.v1.state", 1,
                      Json{{"status", "offline"},
                           {"mode", "auto"},
                           {"channels", Json::array()},
                           {"actuators", Json::array()},
                           {"fw_version", "1.0"}}),
             "state");
  EXPECT_EQ(codes(out), std::vector<std::string>{"DEVICE_OFFLINE"});
  out = online_state(2);
  EXPECT_EQ(codes(out), std::vector<std::string>{"DEVICE_ONLINE"});
}

TEST_F(CoreMqttTest, OwnMessagesAreIgnored) {
  auto own = envelope("iot.v1.state", 0, Json{{"status", "online"}});
  own["device_id"] = "gw-01";
  (void)mqtt(own, "state");
  own["device_id"] = "mcu-c-01";
  (void)mqtt(own, "state");
  EXPECT_EQ(core.stats().devices_registered, 0U);
  EXPECT_EQ(core.registry().find("gw-01"), nullptr);
}

TEST_F(CoreMqttTest, UartTelemetryAlsoUpdatesRegistry) {
  (void)core.step(gw::core::UartReceived{u::Decoded{u::Frame{u::State{0, 0x0102, 1, 1000}}},
                                         clock.system_now()});
  (void)core.step(gw::core::UartReceived{u::Decoded{u::Frame{u::Telemetry{0, 10, 2512, 0, 1, 0}}},
                                         clock.system_now()});
  EXPECT_EQ(core.registry().find("mcu-c-01")->channels.at("temp_board").value, 25.12);
  EXPECT_TRUE(core.uart_online());
}
