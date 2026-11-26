// UartBridge（iotspec の tests/unit/test_uart.py の bridge のテストと同じ観点）と SeqExtender
#include <gtest/gtest.h>

#include "core/common/Clock.h"
#include "core/common/Constants.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"
#include "core/uart/SeqExtender.h"
#include "core/uart/UartBridge.h"

namespace u = gw::core::uart;
namespace c = gw::core::constants;
using gw::core::Json;

namespace {

const gw::core::SystemTime kT0 = *gw::core::parse_ts("2026-11-20T06:00:00.000Z");

class BridgeTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{kT0};
  gw::core::SequentialIdGenerator ids{clock};
  u::UartBridge bridge{u::load_profile(GW_PROFILES_DIR, "mcu-c-01"), ids, std::string{"gw-01"}};

  u::BridgeResult rx(const u::Frame& frame) { return bridge.receive(frame, clock.system_now()); }
  void boot(std::uint32_t boot_id = 0xA1B2C3D4) {
    ASSERT_TRUE(rx(u::State{0, 0x0102, boot_id, 1000}).envelope.has_value());
  }
};

}  // namespace

TEST(SeqExtender, WrapsAndIsMonotonic) {
  u::SeqExtender ext;
  EXPECT_EQ(ext.extend(65534), 65534U);
  EXPECT_EQ(ext.extend(65535), 65535U);
  EXPECT_EQ(ext.extend(0), 65536U);
  EXPECT_EQ(ext.extend(0), 65536U);  // 同じ値は一周とみなさない
  EXPECT_EQ(ext.extend(5), 65541U);
  ext.reset();
  EXPECT_EQ(ext.extend(3), 3U);
}

TEST_F(BridgeTest, StateIsCompletedFromProfileWithBridgedBy) {
  const auto result = rx(u::State{0, 0x0203, 0xA1B2C3D4, 250});
  ASSERT_TRUE(result.envelope);
  const auto msg = result.envelope->to_json();
  EXPECT_EQ(msg["boot_id"], "a1b2c3d4");
  EXPECT_EQ(msg["ts_source"], "gateway");
  const auto& p = msg["payload"];
  EXPECT_EQ(p["mode"], "INIT");
  EXPECT_EQ(p["fw_version"], "2.3");
  EXPECT_EQ(p["bridged_by"], "gw-01");
  EXPECT_EQ(p["channels"][0]["period_ms"], 250);
  EXPECT_TRUE(p["actuators"][0]["max_on_s"].is_null());
  EXPECT_FALSE(msg.contains("uptime_ms"));
}

TEST_F(BridgeTest, AwaitingStateDiscardsEverythingAndAsksOnce) {
  const auto first = rx(u::Telemetry{0, 10, 2000, 0, 1, 0});
  EXPECT_EQ(first.discarded, "awaiting_state");
  EXPECT_TRUE(first.request_state);
  EXPECT_EQ(rx(u::Event{0, 20, 2, 0, 1, 2, 2000}).discarded, "awaiting_state");
  EXPECT_FALSE(rx(u::Ack{0, 7, 1, 0, 0, 0}).request_state);
  EXPECT_EQ(rx(u::Diag{0, 30, 1, 0, 0, 0, 0, 0}).discarded, "awaiting_state");
  EXPECT_EQ(bridge.dropped_telemetry(), 1U);
  EXPECT_EQ(bridge.discards().at("awaiting_state"), 4U);
}

TEST_F(BridgeTest, RebootDetectionDropsTelemetryUntilState) {
  boot();
  EXPECT_TRUE(rx(u::Telemetry{1, 5000, 2000, 0, 1, 0}).envelope);
  const auto detected = rx(u::Telemetry{2, 100, 2000, 0, 1, 0});
  EXPECT_EQ(detected.discarded, "awaiting_state");
  EXPECT_TRUE(detected.request_state);
  EXPECT_FALSE(rx(u::Telemetry{3, 1100, 2000, 0, 1, 0}).request_state);
  const auto state = rx(u::State{5, 0x0102, 0x2, 1000});
  ASSERT_TRUE(state.envelope);
  EXPECT_EQ(state.envelope->seq, 5U);  // 新しい起動として連番を振り直す
  EXPECT_EQ(bridge.boot_id(), "00000002");
  EXPECT_EQ(bridge.dropped_telemetry(), 2U);
}

TEST_F(BridgeTest, DiagIsNotConvertedAndCarriesFields) {
  boot();
  clock.advance(gw::core::Millis{2000});
  const auto result = rx(u::Diag{0, 4321, 3, 2, 1, 0xFFFF, 900, 7});
  EXPECT_FALSE(result.envelope);
  EXPECT_EQ(result.discarded, "not_converted");
  ASSERT_TRUE(result.diag);
  EXPECT_EQ((*result.diag)["received_at"], "2026-11-20T06:00:02.000Z");
  EXPECT_EQ((*result.diag)["boot_id"], "a1b2c3d4");
  EXPECT_EQ((*result.diag)["rx_frame_errors"], 0xFFFF);
  // DIAG の uptime_ms の減少も再起動の検知に使う
  EXPECT_TRUE(rx(u::Diag{1, 10, 1, 0, 0, 0, 0, 0}).request_state);
}

TEST_F(BridgeTest, BootDetailHasOnlyResetReason) {
  boot();
  const std::vector<std::string> names{"unknown",  "power_on", "pin",      "watchdog",
                                       "software", "brownout", "low_power"};
  for (std::size_t n = 0; n < names.size(); ++n) {
    const auto i = static_cast<std::uint8_t>(n);
    const auto result = rx(u::Event{i, static_cast<std::uint32_t>(10 + i), 1, 0, i, 0, 2500});
    ASSERT_TRUE(result.envelope);
    EXPECT_EQ(result.envelope->payload["detail"], Json({{"reset_reason", names[n]}}));
  }
  EXPECT_EQ(rx(u::Event{7, 100, 1, 0, 7, 0, 2500}).discarded, "unknown_value");
  EXPECT_EQ(rx(u::Event{8, 110, 1, 0, 1, 1, 2500}).discarded, "unknown_value");  // to ≠ 0
}

TEST_F(BridgeTest, InterlockEventsKeepFromAndToState) {
  boot();
  const auto active = rx(u::Event{0, 10, 9, 1, 3, 3, 6100});
  ASSERT_TRUE(active.envelope);
  EXPECT_EQ(active.envelope->payload["code"], "INTERLOCK_ACTIVE");
  EXPECT_EQ(active.envelope->payload["severity"], "warning");
  EXPECT_EQ(active.envelope->payload["detail"]["from_state"], "CRITICAL");
  const auto released = rx(u::Event{1, 20, 10, 0, 1, 1, 3000});
  ASSERT_TRUE(released.envelope);
  EXPECT_EQ(released.envelope->payload["code"], "INTERLOCK_RELEASED");
  EXPECT_EQ(released.envelope->payload["detail"]["to_state"], "NORMAL");
}

TEST_F(BridgeTest, UnknownValuesAndSchemaViolationsAreDiscarded) {
  boot();
  EXPECT_EQ(rx(u::Telemetry{1, 10, 2000, 9, 1, 0}).discarded, "unknown_value");
  EXPECT_EQ(rx(u::Telemetry{2, 20, 2000, 0, 99, 0}).discarded, "unknown_value");
  EXPECT_EQ(rx(u::Event{3, 30, 99, 0, 1, 1, 2000}).discarded, "unknown_value");
  EXPECT_EQ(rx(u::Ack{4, 0x4242, 1, 0, 0, 0}).discarded, "unknown_cmd_ref");
  EXPECT_EQ(rx(u::Telemetry{5, 40, 2000, 0, 1, 1001}).discarded, "schema_invalid");
  EXPECT_EQ(rx(u::Cmd{}).discarded, "unexpected_direction");
  const Json get_status =
      Json::parse(R"({"cmd_id":"01JD3R0A5K7M2N4P6R8T0V2X4Y","op":"get_status"})");
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(get_status, 9)));
  EXPECT_EQ(rx(u::Ack{6, 9, 2, 0, 0, 0}).discarded, "schema_invalid");  // rejected で reason null
  EXPECT_EQ(rx(u::Ack{7, 9, 1, 0, 2, 0}).discarded, "unknown_value");   // limited が 0/1 以外
}

TEST_F(BridgeTest, AckAppliedOnlyForOutputOpAndAcceptedOrDone) {
  boot();
  const Json set = Json::parse(
      R"({"cmd_id":"01JD3R0A5K7M2N4P6R8T0V2X4Y","op":"set_actuator","args":{"id":"fan_main","level":900}})");
  const Json rel = Json::parse(
      R"({"cmd_id":"01JD3R0B5K7M2N4P6R8T0V2X4Y","op":"release","args":{"id":"fan_main"}})");
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(set, 10)));
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(rel, 11)));
  const auto done = rx(u::Ack{1, 10, 1, 0, 1, 700});
  ASSERT_TRUE(done.envelope);
  EXPECT_EQ(done.envelope->payload["applied"], Json({{"level", 700}}));
  EXPECT_EQ(done.envelope->payload["limited"], true);
  const auto rejected = rx(u::Ack{2, 10, 2, 2, 0, 0});  // v0.2.2 #44
  ASSERT_TRUE(rejected.envelope);
  EXPECT_FALSE(rejected.envelope->payload.contains("applied"));
  const auto released = rx(u::Ack{3, 11, 1, 0, 0, 0});
  ASSERT_TRUE(released.envelope);
  EXPECT_FALSE(released.envelope->payload.contains("applied"));
}

TEST_F(BridgeTest, InternalGetDiagAckIsNotConverted) {
  boot();
  const auto frame = bridge.internal_frame(c::kUartOpGetDiag, 0x42);
  EXPECT_EQ(frame.ttl_s, 0);
  const auto result = rx(u::Ack{0, 0x42, 1, 0, 0, 0});
  EXPECT_FALSE(result.envelope);
  EXPECT_EQ(result.discarded, "not_converted");
  ASSERT_TRUE(result.internal_ack);
  EXPECT_EQ((*result.internal_ack)["op"], "GET_DIAG");
  EXPECT_EQ((*result.internal_ack)["status"], "done");
  // 同じ cmd_ref を汎用指令に使い直したら cmd/ack にする
  const Json get_status =
      Json::parse(R"({"cmd_id":"01JD3R0A5K7M2N4P6R8T0V2X4Y","op":"get_status"})");
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(get_status, 0x42)));
  EXPECT_TRUE(rx(u::Ack{1, 0x42, 1, 0, 0, 0}).envelope);
  EXPECT_THROW((void)bridge.internal_frame(c::kUartOpSetFanOverride, 1), std::invalid_argument);
  EXPECT_THROW((void)bridge.internal_frame(c::kUartOpSetThreshold, 1, {1, 2}),
               std::invalid_argument);
}

TEST_F(BridgeTest, CommandToFrameMapsAndRejects) {
  const auto ok = bridge.command_to_frame(
      Json::parse(
          R"({"cmd_id":"A","op":"set_actuator","args":{"id":"fan_main","level":1000},"lease_s":600})"),
      1);
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(ok));
  EXPECT_EQ(std::get<u::Cmd>(ok).ttl_s, 600);
  EXPECT_EQ(std::get<u::Cmd>(ok).args, std::vector<std::int32_t>{1000});
  const auto omitted = bridge.command_to_frame(
      Json::parse(R"({"cmd_id":"B","op":"set_actuator","args":{"id":"fan_main","level":0}})"), 2);
  EXPECT_EQ(std::get<u::Cmd>(omitted).ttl_s, 60);
  const auto reason = [&](const char* text) {
    const auto r = bridge.command_to_frame(Json::parse(text), 3);
    return std::holds_alternative<u::CommandRejected>(r) ? std::get<u::CommandRejected>(r).reason
                                                         : std::string{"(変換できた)"};
  };
  EXPECT_EQ(reason(R"({"cmd_id":"C","op":"set_mode","args":{"mode":"auto"}})"), "unknown_op");
  EXPECT_EQ(reason(R"({"cmd_id":"C","op":"set_actuator","args":{"id":"pump","level":1}})"),
            "unknown_target");
  EXPECT_EQ(reason(R"({"cmd_id":"C","op":"set_actuator","args":{"id":"fan_main"}})"),
            "invalid_arg");
  EXPECT_EQ(
      reason(
          R"({"cmd_id":"C","op":"set_actuator","args":{"id":"fan_main","level":1},"lease_s":0})"),
      "invalid_arg");
  EXPECT_EQ(
      reason(
          R"({"cmd_id":"C","op":"set_actuator","args":{"id":"fan_main","level":1},"lease_s":601})"),
      "invalid_arg");
  EXPECT_EQ(
      reason(
          R"({"cmd_id":"C","op":"set_actuator","args":{"id":"fan_main","level":1},"lease_s":true})"),
      "invalid_arg");
}

TEST_F(BridgeTest, OfflineStateDoesNotAdvanceSeq) {
  EXPECT_FALSE(bridge.offline_state(clock.system_now()).has_value());  // STATE 未受信
  boot();
  const auto offline = bridge.offline_state(clock.system_now());
  ASSERT_TRUE(offline);
  EXPECT_EQ(offline->seq, 1U);  // STATE の seq 0 の次（進めない）
  EXPECT_EQ(offline->payload["status"], "offline");
  EXPECT_EQ(offline->payload["bridged_by"], "gw-01");
  const auto next = rx(u::State{1, 0x0102, 0xA1B2C3D4, 1000});
  ASSERT_TRUE(next.envelope);
  EXPECT_EQ(next.envelope->seq, 1U);
}

TEST_F(BridgeTest, PerTypeSeqHasNoFalseGaps) {
  boot();
  const Json get_status =
      Json::parse(R"({"cmd_id":"01JD3R0A5K7M2N4P6R8T0V2X4Y","op":"get_status"})");
  ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(get_status, 1)));
  for (std::uint16_t i = 0; i < 5; ++i) {
    const auto t = rx(u::Telemetry{i, i * 1000U + 1000U, 2000, 0, 1, 0});
    EXPECT_FALSE(rx(u::Heartbeat{i, i * 1000U + 1001U}).envelope);
    const auto e = rx(u::Event{i, i * 1000U + 1002U, 2, 0, 1, 2, 2000});
    ASSERT_TRUE(t.envelope && e.envelope);
    EXPECT_EQ(t.envelope->seq, i);
    EXPECT_EQ(e.envelope->seq, i);
  }
  EXPECT_TRUE(bridge.uart_gaps().empty());
  (void)rx(u::Telemetry{8, 9000, 2000, 0, 1, 0});
  EXPECT_EQ(bridge.uart_gaps().at("TELEMETRY"), 3U);
}
