// JSON Schema の検証（D6、jsoncons）
// - samples/valid 全件が通り、samples/invalid 全件が拒否される（iotspec の Python jsonschema と同じ判定）
// - open-questions #9：ゴールデン35件から作った UART 由来の電文と、その値を UART の取りうる範囲で変えたものを
//   スキーマで検証し、UartBridge の schema_violation の判定と一致する
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "adapters/mqtt/SchemaValidator.h"
#include "core/common/Clock.h"
#include "core/common/Constants.h"
#include "core/common/Ids.h"
#include "core/uart/FrameCodec.h"
#include "core/uart/Mapping.h"
#include "core/uart/Profile.h"
#include "core/uart/UartBridge.h"

namespace fs = std::filesystem;
namespace u = gw::core::uart;
using gw::adapters::SchemaValidator;
using gw::core::Json;

namespace {

const fs::path kSpec{GW_IOTSPEC_DIR};

const SchemaValidator& validator() {
  static const SchemaValidator v(kSpec / "schemas");
  return v;
}

std::string read(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::vector<fs::path> files(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ".json") {
      out.push_back(e.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(Schema, EveryValidSamplePasses) {
  const auto valid = files(kSpec / "samples" / "valid");
  ASSERT_GE(valid.size(), 38U);
  for (const auto& p : valid) {
    const auto r = validator().validate(read(p));
    EXPECT_TRUE(r.ok) << p.filename() << ": " << r.reason << " " << r.detail;
  }
}

TEST(Schema, EveryInvalidSampleIsRejected) {
  const auto invalid = files(kSpec / "samples" / "invalid");
  ASSERT_GE(invalid.size(), 54U);
  for (const auto& p : invalid) {
    const auto r = validator().validate(read(p));
    EXPECT_FALSE(r.ok) << p.filename();
    if (p.filename().string().starts_with("envelope__unknown_schema")) {
      EXPECT_EQ(r.reason, "unknown_schema");
    }
  }
}

TEST(Schema, ReasonsAndKind) {
  EXPECT_EQ(validator().validate("{not json").reason, "json_invalid");
  EXPECT_EQ(validator().validate("[1,2]").reason, "json_invalid");
  EXPECT_EQ(validator().validate(R"({"schema":"iot.v9.x"})").reason, "unknown_schema");
  const auto ok = validator().validate(read(kSpec / "samples/valid/cmd_ack__accepted.json"));
  ASSERT_TRUE(ok.ok);
  EXPECT_EQ(ok.kind, "cmd/ack");
  EXPECT_EQ(ok.schema, "iot.v1.cmd_ack");
  EXPECT_TRUE(ok.message.has_value());
}

// #9：UartBridge が作る電文の判定（schema_violation）とスキーマの判定が一致する
TEST(Schema, UartBridgeSchemaChecksAgreeWithJsonSchema) {
  gw::core::VirtualClock clock(gw::core::SystemTime{});
  gw::core::SequentialIdGenerator ids(clock);
  u::UartBridge bridge(u::load_profile(GW_PROFILES_DIR, "mcu-c-01"), ids, std::string{"gw-01"});
  int converted = 0;
  int expected_envelopes = 0;
  int compared = 0;
  // ゴールデン35件を1本のセッションとして流し、電文になったものはすべてスキーマに適合する
  for (const auto& p : files(kSpec / "uart" / "golden")) {
    const auto g = nlohmann::json::parse(read(p));
    if (g["direction"] == "03->01") {
      if (g.contains("input_cmd_payload")) {
        (void)bridge.command_to_frame(Json::parse(g["input_cmd_payload"].dump()),
                                      g["cmd_ref"].get<std::uint16_t>());
      } else if (g.contains("internal_op")) {
        (void)bridge.internal_frame(gw::core::constants::kUartOpGetDiag, g["cmd_ref"]);
      }
      continue;
    }
    std::vector<std::uint8_t> data;
    std::istringstream hex(g["hex"].get<std::string>());
    std::string byte;
    while (hex >> byte) {
      data.push_back(static_cast<std::uint8_t>(std::stoul(byte, nullptr, 16)));
    }
    expected_envelopes += g["expected"]["envelope"].is_null() ? 0 : 1;
    const auto at = *gw::core::parse_ts(g["received_at"].get<std::string>());
    const auto result = bridge.receive(u::decode_frame(data), at);
    if (result.envelope) {
      ++converted;
      EXPECT_TRUE(validator().is_valid(result.envelope->to_json())) << p.filename();
      EXPECT_FALSE(u::schema_violation(*result.envelope).has_value()) << p.filename();
    }
  }
  EXPECT_EQ(converted, expected_envelopes);
  EXPECT_GT(converted, 0);
  // UART の値の取りうる範囲で変えた電文：fan_pm（u16）と、ACK の status×reason×applied
  const Json set_cmd = Json::parse(
      R"({"cmd_id":"01JD3R0A5K7M2N4P6R8T0V2X4Y","op":"set_actuator","args":{"id":"fan_main","level":1}})");
  for (const std::uint16_t fan : {0, 1, 999, 1000, 1001, 4096, 65535}) {
    (void)bridge.receive(u::Frame{u::State{0, 0x0102, 0x1, 1000}}, gw::core::SystemTime{});
    const auto r =
        bridge.receive(u::Frame{u::Telemetry{1, 10, 2500, 0, 1, fan}}, gw::core::SystemTime{});
    // 破棄されたものも、電文の形にして確かめる（schema_violation が理由を返す）
    Json payload = Json::parse(
        R"({"readings":[{"channel":"temp_board","value":25.0,"unit":"degC","quality":"good"}],
      "device_state":"NORMAL","actuators":[{"id":"fan_main","level":0}]})");
    payload["actuators"][0]["level"] = fan;
    gw::core::Envelope env;
    env.schema = "iot.v1.telemetry";
    env.msg_id = "01JD3R0A5K7M2N4P6R8T0V2X4Y";
    env.device_id = "mcu-c-01";
    env.boot_id = "00000001";
    env.ts_source = "gateway";
    env.payload = payload;
    EXPECT_EQ(u::schema_violation(env).has_value(), !validator().is_valid(env.to_json())) << fan;
    EXPECT_EQ(r.discarded.value_or("") == "schema_invalid", fan > 1000) << fan;
    ++compared;
  }
  for (std::uint8_t status = 0; status < 4; ++status) {
    for (std::uint8_t reason = 0; reason < 6; ++reason) {
      ASSERT_TRUE(std::holds_alternative<u::Cmd>(bridge.command_to_frame(set_cmd, 0x77)));
      const auto r = bridge.receive(u::Frame{u::Ack{static_cast<std::uint16_t>(status * 6 + reason),
                                                    0x77, status, reason, 0, 500}},
                                    gw::core::SystemTime{});
      if (r.envelope) {
        EXPECT_TRUE(validator().is_valid(r.envelope->to_json()))
            << int(status) << "/" << int(reason);
      } else {
        ASSERT_EQ(r.discarded, "schema_invalid") << int(status) << "/" << int(reason);
      }
      // 同じ値で組み立てた電文に対するスキーマの判定と一致する
      Json payload{{"cmd_id", "01JD3R0A5K7M2N4P6R8T0V2X4Y"},
                   {"status", std::string{*u::status_table().to_common(status)}},
                   {"limited", false}};
      const auto common = u::reason_table().to_common(reason);
      payload["reason"] = common ? Json(std::string{*common}) : Json(nullptr);
      gw::core::Envelope env;
      env.schema = "iot.v1.cmd_ack";
      env.msg_id = "01JD3R0A5K7M2N4P6R8T0V2X4Y";
      env.device_id = "mcu-c-01";
      env.boot_id = "00000001";
      env.ts_source = "gateway";
      env.payload = payload;
      EXPECT_EQ(u::schema_violation(env).has_value(), !validator().is_valid(env.to_json()))
          << int(status) << "/" << int(reason);
      EXPECT_EQ(r.envelope.has_value(), !u::schema_violation(env).has_value());
      ++compared;
    }
  }
  EXPECT_EQ(compared, 7 + 24);
}
