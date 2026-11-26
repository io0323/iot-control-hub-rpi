// devices.yaml の読み込みと、mqtt-loop の受信（MqttIngest）
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "adapters/config/DevicesConfig.h"
#include "adapters/mqtt/MqttIngest.h"
#include "adapters/mqtt/SchemaValidator.h"
#include "core/common/Clock.h"

using gw::adapters::ConfigError;
using gw::adapters::MqttIngest;
using gw::adapters::MqttMessage;
using gw::adapters::parse_devices_config;

namespace {

std::string sample(const std::string& name) {
  std::ifstream in(std::string{GW_IOTSPEC_DIR} + "/samples/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string errors_of(const std::string& yaml) {
  try {
    (void)parse_devices_config(yaml, "devices.yaml", GW_PROFILES_DIR, "mcu-c-01");
  } catch (const ConfigError& e) {
    return e.what();
  }
  return "(エラーにならない)";
}

}  // namespace

TEST(DevicesConfig, ProfileReferencesAndInlineDevices) {
  const auto config = parse_devices_config(R"(devices:
  - profile: mcu-c-01
  - profile: mcu-cpp-01
  - device_id: mcu-cpp-02
    transport: mqtt
    channels: [{channel: temp_air, unit: degC, period_ms: 2000}]
    actuators: []
)",
                                           "devices.yaml", GW_PROFILES_DIR, "mcu-c-01");
  ASSERT_EQ(config.devices.size(), 3U);
  EXPECT_EQ(config.uart_device("mcu-c-01").transport, "uart");
  EXPECT_EQ(config.devices[2].channels.at(0).period_ms, 2000);
  EXPECT_THROW((void)config.uart_device("nobody"), std::out_of_range);
}

TEST(DevicesConfig, ErrorsAreReported) {
  EXPECT_NE(errors_of("devices: [{profile: mcu-cpp-01}]").find("uart.device_id"),
            std::string::npos);
  EXPECT_NE(errors_of("devices: [{profile: mcu-c-01}, {profile: mcu-c-01}]").find("重複"),
            std::string::npos);
  EXPECT_NE(errors_of("devices: [{profile: nope-01}]").find("/devices/0"), std::string::npos);
  EXPECT_NE(errors_of("devices: [{profile: mcu-c-01, transport: mqtt}]").find("ほかの項目"),
            std::string::npos);
  EXPECT_NE(errors_of("devices: [{profile: mcu-c-01}]\nextra: 1").find("/extra"),
            std::string::npos);
  EXPECT_NE(errors_of("nodes: []").find("/devices"), std::string::npos);
  EXPECT_NE(errors_of("devices: [").find("YAML"), std::string::npos);
  EXPECT_NE(errors_of("devices: [{device_id: x-01, transport: mqtt}]").find("/devices/0"),
            std::string::npos);
}

TEST(MqttIngest, ValidMessagesArePushedAndOthersCounted) {
  gw::adapters::SchemaValidator validator(std::string{GW_IOTSPEC_DIR} + "/schemas");
  gw::core::VirtualClock clock(*gw::core::parse_ts("2026-11-26T06:00:00.000Z"));
  std::vector<gw::core::Inbound> pushed;
  bool accept = true;
  MqttIngest ingest(validator, {"gw-01", "mcu-c-01"}, clock, [&](gw::core::Inbound msg) {
    if (accept) {
      pushed.push_back(std::move(msg));
    }
    return accept;
  });
  ingest.handle(MqttMessage{"iot/v1/home/mcu-cpp-01/telemetry",
                            sample("valid/telemetry__all_units_and_qualities.json"), false});
  ingest.handle(
      MqttMessage{"iot/v1/home/mcu-cpp-01/state", sample("valid/state__online.json"), true});
  ingest.handle(MqttMessage{"iot/v1/home/mcu-cpp-01/telemetry",
                            sample("invalid/telemetry__unknown_unit.json"), false});
  ingest.handle(MqttMessage{"iot/v1/home/mcu-cpp-01/telemetry", "{oops", false});
  ingest.handle(
      MqttMessage{"iot/v1/home/mcu-c-01/state", sample("valid/state__online.json"), false});
  ingest.handle(MqttMessage{"iot/v1/home/gw-01/event", "{}", false});
  ingest.handle(
      MqttMessage{"iot/v1/home/app-01/request", sample("valid/request__spec_example.json"), false});
  accept = false;
  ingest.handle(MqttMessage{"iot/v1/home/mcu-cpp-01/event",
                            sample("valid/event__threshold_exceeded_full.json"), false});
  ASSERT_EQ(pushed.size(), 3U);
  EXPECT_EQ(std::get<gw::core::MqttReceived>(pushed[2]).kind, "request");  // Phase 3：core に渡す
  const auto& first = std::get<gw::core::MqttReceived>(pushed[0]);
  EXPECT_EQ(first.kind, "telemetry");
  EXPECT_FALSE(first.retained);
  EXPECT_TRUE(std::get<gw::core::MqttReceived>(pushed[1]).retained);
  const auto stats = ingest.stats();
  EXPECT_EQ(stats.received, 8U);
  EXPECT_EQ(stats.accepted, 3U);
  EXPECT_EQ(stats.own_echo, 2U);
  EXPECT_EQ(stats.requests, 1U);
  EXPECT_EQ(stats.inbound_dropped, 1U);
  EXPECT_EQ(stats.rejected.at("schema_invalid"), 1U);
  EXPECT_EQ(stats.rejected.at("json_invalid"), 1U);
  EXPECT_EQ(stats.validated, 6U);
}

TEST(MqttIngest, Subscriptions) {
  const auto topics = MqttIngest::subscriptions("home");
  EXPECT_EQ(topics, (std::vector<std::string>{"iot/v1/home/+/telemetry", "iot/v1/home/+/event",
                                              "iot/v1/home/+/state", "iot/v1/home/+/cmd/ack",
                                              "iot/v1/home/app-01/request"}));
}
