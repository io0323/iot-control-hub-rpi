// ゴールデン照合（詳細設計書 第8章の1段目）。
// uart/golden/*.json を NN 順に1つの UartBridge に流す
// （gen_golden.py の _Session と同じ1本のセッション）。
// 仮想時計に received_at を入れ、SequentialIdGenerator で msg_id を揃える。
// - 01→03：エンベロープ全体（msg_id・ts を含む）、discarded、request_state、
//   dropped_telemetry_total、diag
// - 03→01：input_cmd_payload と cmd_ref（または internal_op）から作ったフレームの hex
// テストはゴールデンのファイルごとに1件。失敗したら番号・名前と、食い違った項目を表示する。
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/common/Clock.h"
#include "core/common/Constants.h"
#include "core/common/Ids.h"
#include "core/uart/FrameCodec.h"
#include "core/uart/Profile.h"
#include "core/uart/UartBridge.h"

namespace {

namespace fs = std::filesystem;
using gw::core::uart::Bytes;
using nlohmann::json;

const fs::path kGoldenDir = fs::path(GW_IOTSPEC_DIR) / "uart" / "golden";

std::vector<fs::path> golden_files() {
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(kGoldenDir)) {
    if (entry.path().extension() == ".json") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

json read_json(const fs::path& path) {
  std::ifstream in(path);
  return json::parse(in);
}

std::string hex(const Bytes& data) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i != 0) {
      out += ' ';
    }
    out += kDigits[data[i] >> 4U];
    out += kDigits[data[i] & 0xFU];
  }
  return out;
}

Bytes from_hex(const std::string& text) {
  Bytes out;
  std::istringstream in(text);
  std::string byte;
  while (in >> byte) {
    out.push_back(static_cast<std::uint8_t>(std::stoul(byte, nullptr, 16)));
  }
  return out;
}

std::uint8_t op_code(const std::string& name) {
  for (const auto& [code, op_name] : gw::core::constants::kUartOpNames) {
    if (op_name == name) {
      return code;
    }
  }
  throw std::runtime_error("知らない op: " + name);
}

// 順序によらず比べる（Python の dict の比較と同じ）
json normalised(const gw::core::Json& value) {
  return json::parse(value.dump());
}

struct Outcome {
  std::string label;
  std::vector<std::string> problems;
};

void expect_eq(Outcome& o, const std::string& item, const json& want, const json& got) {
  if (want != got) {
    o.problems.push_back(item + "\n  期待: " + want.dump() + "\n  実際: " + got.dump());
  }
}

// ゴールデン全件を1本のセッションとして流した結果（ファイル名 → 結果）
const std::map<std::string, Outcome>& session() {
  static const std::map<std::string, Outcome> results = [] {
    std::map<std::string, Outcome> out;
    gw::core::VirtualClock clock(gw::core::SystemTime{});
    gw::core::SequentialIdGenerator ids(clock);
    gw::core::uart::UartBridge bridge(
        gw::core::uart::load_profile(GW_PROFILES_DIR,
                                     std::string{gw::core::constants::kDeviceMcuC01}),
        ids, std::string{gw::core::constants::kDeviceGw01});
    for (const auto& path : golden_files()) {
      const auto g = read_json(path);
      Outcome o;
      o.label = std::to_string(g["order"].get<int>()) + "番 " + g["name"].get<std::string>();
      const auto& frame = g["frame"];
      const std::string want_hex = g["hex"];
      try {
        if (g["direction"] == "03->01") {
          Bytes built;
          if (g.contains("input_cmd_payload")) {
            const auto payload = gw::core::Json::parse(g["input_cmd_payload"].dump());
            auto result = bridge.command_to_frame(payload, g["cmd_ref"].get<std::uint16_t>());
            if (const auto* rejected = std::get_if<gw::core::uart::CommandRejected>(&result)) {
              o.problems.push_back("CMD に変換できない: " + rejected->reason);
            } else {
              built = gw::core::uart::encode_frame(std::get<gw::core::uart::Cmd>(result));
            }
          } else if (g.contains("internal_op")) {
            built = gw::core::uart::encode_frame(bridge.internal_frame(
                op_code(g["internal_op"]), g["cmd_ref"].get<std::uint16_t>()));
          } else if (frame["type"] == "HEARTBEAT") {
            built = gw::core::uart::encode_frame(bridge.heartbeat_frame(frame["uptime_ms"]));
          } else {
            // 汎用指令との対応がない CMD（符号化だけを照合する）
            gw::core::uart::Cmd cmd;
            cmd.seq = bridge.next_tx_seq(gw::core::uart::Cmd::kType);
            expect_eq(o, "frame.seq（03→01 の CMD の連番）", frame["seq"], cmd.seq);
            cmd.cmd_ref = frame["cmd_ref"];
            cmd.op = frame["op"];
            cmd.ttl_s = frame["ttl_s"];
            cmd.args = frame["args"].get<std::vector<std::int32_t>>();
            built = gw::core::uart::encode_frame(cmd);
          }
          expect_eq(o, "hex", want_hex, hex(built));
        } else {
          const auto& expected = g["expected"];
          const auto at = gw::core::parse_ts(g["received_at"].get<std::string>());
          if (!at) {
            throw std::runtime_error("received_at を読めない");
          }
          clock.set_system(*at);
          const auto data = from_hex(want_hex);
          const auto decoded = gw::core::uart::decode_frame(data);
          if (const auto* f = std::get_if<gw::core::uart::Frame>(&decoded)) {
            // C++ の符号化も同じ hex になる
            expect_eq(o, "hex（frame を符号化）", want_hex, hex(gw::core::uart::encode_frame(*f)));
          }
          const auto result = bridge.receive(decoded, *at);
          expect_eq(o, "expected.envelope", expected["envelope"],
                    result.envelope ? normalised(result.envelope->to_json()) : json(nullptr));
          expect_eq(o, "expected.discarded", expected["discarded"],
                    result.discarded ? json(*result.discarded) : json(nullptr));
          expect_eq(o, "expected.request_state", expected["request_state"], result.request_state);
          expect_eq(o, "expected.dropped_telemetry_total", expected["dropped_telemetry_total"],
                    bridge.dropped_telemetry());
          expect_eq(o, "expected.diag", expected.value("diag", json(nullptr)),
                    result.diag ? normalised(*result.diag) : json(nullptr));
        }
      } catch (const std::exception& e) {
        o.problems.push_back(std::string{"例外: "} + e.what());
      }
      out[path.stem().string()] = std::move(o);
    }
    return out;
  }();
  return results;
}

std::vector<std::string> golden_names() {
  std::vector<std::string> names;
  for (const auto& path : golden_files()) {
    names.push_back(path.stem().string());
  }
  return names;
}

class Golden : public ::testing::TestWithParam<std::string> {};

TEST_P(Golden, MatchesReferenceImplementation) {
  const auto& results = session();
  const auto it = results.find(GetParam());
  ASSERT_NE(it, results.end());
  std::string message;
  for (const auto& p : it->second.problems) {
    message += "\n- " + p;
  }
  EXPECT_TRUE(it->second.problems.empty()) << it->second.label << message;
}

INSTANTIATE_TEST_SUITE_P(V022, Golden, ::testing::ValuesIn(golden_names()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           std::string name = "g" + info.param;
                           std::replace_if(
                               name.begin(), name.end(),
                               [](char c) {
                                 return std::isalnum(static_cast<unsigned char>(c)) == 0;
                               },
                               '_');
                           return name;
                         });

TEST(GoldenSet, HasAllThirtyFiveFilesOfV022) {
  EXPECT_EQ(golden_names().size(), 35U) << kGoldenDir;
}

}  // namespace
