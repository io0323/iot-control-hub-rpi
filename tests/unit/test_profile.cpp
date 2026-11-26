#include <gtest/gtest.h>

#include "core/common/Constants.h"
#include "core/uart/Profile.h"

namespace u = gw::core::uart;

TEST(Profile, BundledMcuC01Loads) {
  const auto p = u::load_profile(GW_PROFILES_DIR, "mcu-c-01");
  EXPECT_EQ(p.transport, "uart");
  EXPECT_EQ(p.state_name(4), "SENSOR_FAULT");
  ASSERT_EQ(p.channels.size(), 1U);
  EXPECT_EQ(p.channels[0].period_ms, 1000);
  ASSERT_EQ(p.actuators.size(), 1U);
  EXPECT_TRUE(p.actuators[0].min.is_number_integer());
  EXPECT_TRUE(p.actuators[0].max_on_s.is_null());
  const auto* set = p.find_command("set_actuator", std::string{"fan_main"});
  ASSERT_NE(set, nullptr);
  EXPECT_EQ(set->uart_op, gw::core::constants::kUartOpSetFanOverride);
  EXPECT_TRUE(set->ttl_from_lease);
  EXPECT_EQ(p.find_command("set_actuator", std::string{"pump"}), nullptr);
  EXPECT_NE(p.find_command("get_status", std::nullopt), nullptr);
  EXPECT_THROW((void)p.state_name(9), std::runtime_error);
}

TEST(Profile, AllBundledProfilesLoad) {
  for (const auto* id : {"gw-01", "mcu-c-01", "mcu-cpp-01", "app-01"}) {
    EXPECT_NO_THROW((void)u::load_profile(GW_PROFILES_DIR, id)) << id;
  }
}

TEST(Profile, InvalidProfilesAreRejectedWithReason) {
  const auto errors = [](const std::string& text) -> std::string {
    try {
      (void)u::parse_profile(text, "t");
    } catch (const u::ProfileError& e) {
      return e.what();
    }
    return "(エラーにならない)";
  };
  const std::string base =
      "device_id: mcu-c-01\ntransport: uart\nchannels: []\nactuators: [{id: fan_main, min: 0, max: "
      "1000}]\nstates: {0: INIT}\n";
  EXPECT_NE(errors(base + "commands: [{op: fly, uart_op: GET_STATUS}]").find("/commands/0/op"),
            std::string::npos);
  EXPECT_NE(errors(base + "commands: [{op: set_actuator, target: pump, uart_op: SET_FAN_OVERRIDE, "
                          "args: {duty_pm: level}}]")
                .find("/commands/0/target"),
            std::string::npos);
  EXPECT_NE(
      errors(base + "commands: [{op: set_actuator, target: fan_main, uart_op: SET_FAN_OVERRIDE}]")
          .find("/commands/0/args"),
      std::string::npos);
  EXPECT_NE(
      errors("device_id: mcu-c-01\ntransport: uart\nchannels: []\nactuators: []\n").find("states"),
      std::string::npos);
  EXPECT_NE(
      errors("device_id: X\ntransport: mqtt\nchannels: []\nactuators: []\n").find("/device_id"),
      std::string::npos);
  EXPECT_NE(errors("device_id: gw-01\ntransport: mqtt\nchannels: [{channel: t, unit: kelvin, "
                   "period_ms: 1}]\nactuators: []\n")
                .find("/channels/0/unit"),
            std::string::npos);
  EXPECT_NE(errors("device_id: gw-01\ntransport: mqtt\nchannels: []\nactuators: []\nextra: 1\n")
                .find("extra"),
            std::string::npos);
}
