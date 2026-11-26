#include <gtest/gtest.h>

#include "adapters/config/GatewayConfig.h"
#include "adapters/config/RequestsConfig.h"
#include "core/uart/Profile.h"

using gw::adapters::ConfigError;
using gw::adapters::parse_gateway_config;

TEST(GatewayConfig, DefaultsAndOverrides) {
  const auto c = parse_gateway_config(
      "site: lab\nmqtt: {host: broker, port: 1884}\nuart: {path: /dev/ttyAMA0}\nprofiles_dir: "
      "share/profiles\n"
      "queues: {inbound: 10}\n",
      "t", "/etc/iot-gw");
  EXPECT_EQ(c.site, "lab");
  EXPECT_EQ(c.mqtt.host, "broker");
  EXPECT_EQ(c.mqtt.port, 1884);
  EXPECT_EQ(c.mqtt.keepalive_s, 60);
  EXPECT_EQ(c.uart.path, "/dev/ttyAMA0");
  EXPECT_EQ(c.uart.baud, 115200);
  EXPECT_EQ(c.uart.device_id, "mcu-c-01");
  EXPECT_EQ(c.profiles_dir, std::filesystem::path("/etc/iot-gw/share/profiles"));
  EXPECT_EQ(c.queues.inbound, 10U);
  EXPECT_EQ(c.queues.uart_out, 64U);
  EXPECT_EQ(c.queues.mqtt_out, 4096U);
  EXPECT_EQ(c.queues.mqtt_hold, 2000U);
  EXPECT_EQ(c.requests_file, std::filesystem::path("/etc/iot-gw/requests.yaml"));
  EXPECT_EQ(parse_gateway_config("requests_file: /opt/r.yaml\n", "t", "/etc/iot-gw").requests_file,
            std::filesystem::path("/opt/r.yaml"));
}

TEST(GatewayConfig, ErrorsListEveryProblem) {
  try {
    (void)parse_gateway_config(
        "site: Lab!\nmqtt: {port: 0, user: x}\nuart: {baud: 12345}\nqueues: {inbound: "
        "0}\nlog_level: loud\nfoo: 1\n",
        "gateway.yaml", ".");
    FAIL() << "ConfigError にならない";
  } catch (const ConfigError& e) {
    const std::string text = e.what();
    for (const auto* item : {"/site", "/mqtt/port", "/mqtt/user", "/uart/baud", "/queues/inbound",
                             "/log_level", "/foo"}) {
      EXPECT_NE(text.find(item), std::string::npos) << item << " がない: " << text;
    }
  }
}

TEST(GatewayConfig, BrokenYaml) {
  EXPECT_THROW((void)parse_gateway_config("site: [", "t", "."), ConfigError);
  EXPECT_THROW((void)gw::adapters::load_gateway_config("/nonexistent/gateway.yaml"), ConfigError);
}

namespace {

std::vector<gw::core::uart::Profile> devices() {
  return {gw::core::uart::load_profile(GW_PROFILES_DIR, "mcu-c-01"),
          gw::core::uart::load_profile(GW_PROFILES_DIR, "mcu-cpp-01")};
}

std::string request_errors(const std::string& yaml) {
  try {
    (void)gw::adapters::parse_requests_config(yaml, "requests.yaml", devices());
  } catch (const ConfigError& e) {
    return e.what();
  }
  return {};
}

}  // namespace

TEST(RequestsConfig, ParsesAllowList) {
  const auto rules = gw::adapters::parse_requests_config(
      "allow:\n"
      "  - {target: mcu-c-01, op: set_actuator, actuators: [fan_main]}\n"
      "  - {target: mcu-cpp-01, op: set_mode}\n"
      "  - {target: other-01, op: release, actuators: [anything]}\n",
      "t", devices());
  ASSERT_EQ(rules.allow.size(), 3U);
  EXPECT_TRUE(rules.permits("mcu-c-01", "set_actuator", "fan_main"));
  EXPECT_FALSE(rules.permits("mcu-c-01", "set_actuator", "led_grow"));
  EXPECT_FALSE(rules.permits("mcu-c-01", "release", "fan_main"));
  EXPECT_TRUE(rules.permits("mcu-cpp-01", "set_mode", ""));
  EXPECT_TRUE(
      rules.permits("other-01", "release", "anything"));  // devices.yaml にない宛先は照合しない
  EXPECT_TRUE(gw::adapters::parse_requests_config("allow: []", "t", devices()).allow.empty());
}

TEST(RequestsConfig, ErrorsListEveryProblem) {
  EXPECT_NE(request_errors("nope: []").find("/allow"), std::string::npos);
  EXPECT_NE(request_errors("allow: [").find("YAML"), std::string::npos);
  EXPECT_NE(request_errors("allow: []\nextra: 1").find("/extra"), std::string::npos);
  EXPECT_NE(request_errors("allow: [1]").find("/allow/0"), std::string::npos);
  EXPECT_NE(request_errors("allow: [{op: set_mode}]").find("/allow/0/target"), std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: a-01}]").find("/allow/0/op"), std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: a-01, op: reboot}]").find("reboot"),
            std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: a-01, op: set_mode, priority: manual}]")
                .find("/allow/0/priority"),
            std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: a-01, op: set_mode, actuators: [x]}]")
                .find("/allow/0/actuators"),
            std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: a-01, op: release, actuators: []}]")
                .find("/allow/0/actuators"),
            std::string::npos);
  EXPECT_NE(request_errors("allow: [{target: mcu-c-01, op: set_actuator, actuators: [pump]}]")
                .find("/allow/0/actuators/0"),
            std::string::npos);
}

TEST(RequestsConfig, SampleConfigIsValid) {
  const auto rules = gw::adapters::load_requests_config(
      std::filesystem::path{GW_SOURCE_DIR} / "config" / "requests.yaml", devices());
  EXPECT_FALSE(rules.allow.empty());
  EXPECT_THROW((void)gw::adapters::load_requests_config("/no/such/requests.yaml", devices()),
               ConfigError);
}
