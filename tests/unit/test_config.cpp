#include <gtest/gtest.h>

#include "adapters/config/GatewayConfig.h"

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
