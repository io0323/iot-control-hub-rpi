#include <gtest/gtest.h>

#include "adapters/config/GatewayConfig.h"
#include "adapters/config/RequestsConfig.h"
#include "adapters/config/RulesConfig.h"
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
  EXPECT_EQ(c.rules_file, std::filesystem::path("/etc/iot-gw/rules.yaml"));
  EXPECT_EQ(c.timezone, "Asia/Tokyo");
  EXPECT_EQ(parse_gateway_config("timezone: UTC\n", "t", "/etc").timezone, "UTC");
  for (const auto* bad : {"timezone: Mars/Olympus\n", "timezone: ../etc/passwd\n"}) {
    try {
      (void)parse_gateway_config(bad, "t", "/etc");
      ADD_FAILURE() << bad;
    } catch (const ConfigError& e) {
      EXPECT_NE(std::string{e.what()}.find("/timezone"), std::string::npos);
    }
  }
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

namespace {

std::string rule_errors(const std::string& yaml) {
  try {
    (void)gw::adapters::parse_rules_config(yaml, "rules.yaml", devices());
  } catch (const ConfigError& e) {
    return e.what();
  }
  return {};
}

}  // namespace

TEST(RulesConfig, Durations) {
  using std::chrono::milliseconds;
  EXPECT_EQ(gw::adapters::parse_duration("500ms"), milliseconds{500});
  EXPECT_EQ(gw::adapters::parse_duration("10s"), milliseconds{10'000});
  EXPECT_EQ(gw::adapters::parse_duration("5m"), milliseconds{300'000});
  EXPECT_EQ(gw::adapters::parse_duration("1h"), milliseconds{3'600'000});
  EXPECT_EQ(gw::adapters::parse_duration("10"), std::nullopt);
  EXPECT_EQ(gw::adapters::parse_duration("1.5s"), std::nullopt);
  EXPECT_EQ(gw::adapters::parse_duration("-1s"), std::nullopt);
}

TEST(RulesConfig, SampleConfigHasTheThreeDesignRules) {
  const auto set = gw::adapters::load_rules_config(
      std::filesystem::path{GW_SOURCE_DIR} / "config" / "rules.yaml", devices());
  ASSERT_EQ(set.rules.size(), 3U);
  const auto& cool = set.rules[0];
  EXPECT_EQ(cool.id, "cool-on-high-temp");
  EXPECT_EQ(cool.priority, gw::core::command::Priority::kAuto);
  EXPECT_EQ(cool.when.kind, gw::core::rules::Condition::Kind::kAny);
  EXPECT_EQ(cool.when.children.size(), 2U);
  EXPECT_EQ(cool.for_duration, std::chrono::seconds{10});
  EXPECT_EQ(cool.cooldown, std::chrono::seconds{30});
  EXPECT_EQ(cool.hysteresis, 2.0);
  const auto& cmd = std::get<gw::core::rules::CommandAction>(cool.then.at(0));
  EXPECT_EQ(cmd.lease_s, 60);
  EXPECT_TRUE(cmd.hold);
  EXPECT_EQ(cmd.args["level"], 800);
  const auto& emergency = set.rules[1];
  EXPECT_EQ(emergency.priority, gw::core::command::Priority::kSafety);
  EXPECT_EQ(emergency.sets_mode(), "EMERGENCY");
  ASSERT_NE(emergency.notify(), nullptr);
  EXPECT_EQ(emergency.notify()->level, "critical");
  const auto& offline = set.rules[2];
  const auto& leaf = std::get<gw::core::rules::StatusLeaf>(offline.when.leaf);
  EXPECT_EQ(leaf.device, "*");
  EXPECT_EQ(leaf.status, gw::core::device::DeviceStatus::kOffline);
  EXPECT_TRUE(std::holds_alternative<gw::core::rules::ReconnectAction>(offline.then.at(0)));
  EXPECT_TRUE(
      gw::adapters::load_rules_config(std::filesystem::path{GW_SOURCE_DIR} / "ci" / "rules.yaml",
                                      devices())
          .rules.empty());
  EXPECT_THROW((void)gw::adapters::load_rules_config("/no/such/rules.yaml", devices()),
               ConfigError);
}

TEST(RulesConfig, AllConditionAndActionShapes) {
  const auto set = gw::adapters::parse_rules_config(R"(
rules:
  - id: x-1
    priority: schedule
    when:
      all:
        - not: {system_mode: EMERGENCY}
        - {time: {from: "22:00", to: "06:30"}}
        - {device: mcu-cpp-01, status: online}
        - {device: other-01, channel: anything, op: "!=", value: 1}
    then:
      - {action: command, device: mcu-cpp-01, op: set_mode, args: {mode: manual}}
      - {action: command, device: mcu-cpp-01, op: release, args: {id: pump}}
      - {action: set_system_mode, mode: ALERT}
      - {action: reconnect}
)",
                                                   "t", devices());
  const auto& rule = set.rules.at(0);
  EXPECT_EQ(rule.when.children.size(), 4U);
  EXPECT_EQ(rule.when.children[0].kind, gw::core::rules::Condition::Kind::kNot);
  const auto& time = std::get<gw::core::rules::TimeLeaf>(rule.when.children[1].leaf);
  EXPECT_EQ(time.from_min, 22 * 60);
  EXPECT_EQ(time.to_min, 6 * 60 + 30);
  EXPECT_EQ(std::get<gw::core::rules::CommandAction>(rule.then[0]).args["mode"], "manual");
  EXPECT_EQ(rule.notify(), nullptr);
}

TEST(RulesConfig, ErrorsListEveryProblem) {
  const auto has = [](const std::string& yaml, const std::string& needle) {
    const auto e = rule_errors(yaml);
    EXPECT_NE(e.find(needle), std::string::npos) << yaml << "\n→ " << e;
  };
  has("nope: []", "/rules");
  has("rules: [", "YAML");
  has("rules: []\nx: 1", "/x");
  has("rules: [1]", "/rules/0");
  const std::string head = "rules:\n  - id: r\n    priority: auto\n";
  const std::string when = "    when: {device: mcu-cpp-01, channel: temp_air, op: \">=\", value: 30}\n";
  const std::string notify = "    then: [{action: notify}]\n";
  has("rules:\n  - {id: R!, priority: auto, when: {system_mode: ALERT}, then: [{action: reconnect}]}",
      "/rules/0/id");
  has(head + when + notify + "  - id: r\n    priority: auto\n" + when + notify, "重複");
  has("rules:\n  - id: r\n    priority: urgent\n" + when + notify, "/rules/0/priority");
  has(head + "    when: {device: mcu-cpp-01, channel: nope, op: \">=\", value: 1}\n" + notify,
      "/rules/0/when/channel");
  has(head + "    when: {device: mcu-cpp-01, channel: temp_air, op: \"=>\", value: 1}\n" + notify,
      "/rules/0/when/op");
  has(head + "    when: {device: \"*\", channel: temp_air, op: \">=\", value: 1}\n" + notify,
      "/rules/0/when/device");
  has(head + "    when: {device: mcu-cpp-01, status: gone}\n" + notify, "/rules/0/when/status");
  has(head + "    when: {system_mode: PANIC}\n" + notify, "/rules/0/when/system_mode");
  has(head + "    when: {time: {from: \"25:00\", to: \"06:00\"}}\n" + notify, "/rules/0/when/time/from");
  has(head + "    when: {all: []}\n" + notify, "/rules/0/when/all");
  has(head + "    when: {foo: 1}\n" + notify, "/rules/0/when");
  has(head + when + "    for: soon\n" + notify, "/rules/0/for");
  has(head + when + "    hysteresis: -1\n" + notify, "/rules/0/hysteresis");
  has(head + when + "    clear_when_below: 2\n" + notify, "/rules/0/clear_when_below");
  has(head + when + "    then: []\n", "/rules/0/then");
  has(head + when + "    then: [{action: notify}, {action: notify}]\n", "notify は1つまで");
  has(head + when + "    then: [{action: notify, level: loud}]\n", "/rules/0/then/0/level");
  has(head + when + "    then: [{action: dance}]\n", "/rules/0/then/0/action");
  has(head + when + "    then: [{action: set_system_mode, mode: NORMAL}]\n", "/rules/0/then/0/mode");
  has(head + when + "    then: [{action: set_system_mode, mode: EMERGENCY}]\n", "safety");
  const std::string cmd = "    then: [{action: command, device: mcu-cpp-01, op: set_actuator, ";
  has(head + when + cmd + "args: {id: nope, level: 1}}]\n", "/rules/0/then/0/args/id");
  has(head + when + cmd + "args: {id: fan_main, level: 1001}}]\n", "/rules/0/then/0/args/level");
  has(head + when + cmd + "args: {id: fan_main, level: 1}, lease: 601s}]\n",
      "/rules/0/then/0/lease");
  has(head + when + cmd + "args: {id: fan_main, level: 1}, lease: 1500ms}]\n",
      "/rules/0/then/0/lease");
  has(head + when + cmd + "args: {id: fan_main, level: 1}, hold: maybe}]\n",
      "/rules/0/then/0/hold");
  has(head + when +
          "    then: [{action: command, device: mcu-cpp-01, op: set_mode, args: {mode: x}, hold: "
          "true}]\n",
      "set_actuator だけ");
  has(head + when + "    then: [{action: command, device: \"*\", op: get_status}]\n",
      "/rules/0/then/0/device");
  has(head + when + "    then: [{action: command, device: mcu-cpp-01, op: reboot}]\n",
      "/rules/0/then/0/op");
}
