// 共通エンベロープ（第3章）と、発信元×kind の seq（iotspec.envelope の Envelope・SeqCounter）。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/common/Clock.h"

namespace gw::core {

using Json = nlohmann::ordered_json;

struct Envelope {
  std::string schema;
  std::string msg_id;
  std::string device_id;
  std::uint32_t seq = 0;
  std::string boot_id;
  SystemTime ts{};
  std::string ts_source;  // "device" または "gateway"
  Json payload = Json::object();
  std::optional<std::uint32_t> uptime_ms;

  // トピックの kind（schema から求める）
  [[nodiscard]] std::string kind() const;
  // 電文の JSON。項目の並びは iotspec の Envelope.to_dict と同じ
  [[nodiscard]] Json to_json() const;
};

// 1つの発信元の kind ごとの連番（uint32、一周したら0）。offline の state では進めない（peek）
class SeqCounter {
 public:
  [[nodiscard]] std::uint32_t next(const std::string& kind);
  [[nodiscard]] std::uint32_t peek(const std::string& kind) const;

 private:
  std::map<std::string, std::uint32_t, std::less<>> next_;
};

}  // namespace gw::core
