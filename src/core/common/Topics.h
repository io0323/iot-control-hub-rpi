// トピック・schema・QoS・retain（第2章のトピック表。iotspec.constants と同じ）。
#pragma once

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gw::core {

inline constexpr std::string_view kTopicRoot = "iot";
inline constexpr std::string_view kTopicMajorVersion = "v1";
inline constexpr std::string_view kTopicFormat = "iot/v1/{site}/{device_id}/{kind}";

struct KindInfo {
  std::string_view kind;    // トピックの kind（例：cmd/ack）
  std::string_view schema;  // エンベロープの schema（例：iot.v1.cmd_ack）
  int qos;
  bool retain;
};

inline constexpr std::array<KindInfo, 7> kKinds{{
    {"telemetry", "iot.v1.telemetry", 0, false},
    {"event", "iot.v1.event", 1, false},
    {"state", "iot.v1.state", 1, true},
    {"cmd", "iot.v1.cmd", 1, false},
    {"cmd/ack", "iot.v1.cmd_ack", 1, false},
    {"request", "iot.v1.request", 1, false},
    {"request/ack", "iot.v1.request_ack", 1, false},
}};

inline constexpr std::string_view kKindTelemetry = "telemetry";
inline constexpr std::string_view kKindEvent = "event";
inline constexpr std::string_view kKindState = "state";
inline constexpr std::string_view kKindCmd = "cmd";
inline constexpr std::string_view kKindCmdAck = "cmd/ack";
inline constexpr std::string_view kKindRequest = "request";
inline constexpr std::string_view kKindRequestAck = "request/ack";

[[nodiscard]] constexpr std::optional<KindInfo> kind_info(std::string_view kind) {
  for (const auto& info : kKinds) {
    if (info.kind == kind) {
      return info;
    }
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<KindInfo> kind_of_schema(std::string_view schema) {
  for (const auto& info : kKinds) {
    if (info.schema == schema) {
      return info;
    }
  }
  return std::nullopt;
}

// 表にある kind（呼び出し側が定数で渡す）。表にない kind は std::invalid_argument
[[nodiscard]] inline KindInfo require_kind(std::string_view kind) {
  const auto info = kind_info(kind);
  if (!info.has_value()) {
    throw std::invalid_argument("unknown kind: " + std::string{kind});
  }
  return info.value();
}

// iot/v1/{site}/{device_id}/{kind}
[[nodiscard]] inline std::string topic_for(std::string_view site, std::string_view device_id,
                                           std::string_view kind) {
  std::string topic;
  topic.reserve(kTopicRoot.size() + kTopicMajorVersion.size() + site.size() + device_id.size() +
                kind.size() + 4);
  topic.append(kTopicRoot).append("/").append(kTopicMajorVersion).append("/");
  topic.append(site).append("/").append(device_id).append("/").append(kind);
  return topic;
}

}  // namespace gw::core
