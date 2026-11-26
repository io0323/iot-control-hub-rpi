// UART フレーム（第7章「種別と対応」）。対応する Python：iotspec.uart.frames
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/common/Constants.h"
#include "core/uart/Cobs.h"

namespace gw::core::uart {

struct Telemetry {
  static constexpr std::uint8_t kType = constants::kUartTypeTelemetry;
  static constexpr std::string_view kName = "Telemetry";
  std::uint16_t seq = 0;
  std::uint32_t uptime_ms = 0;
  std::int16_t temp_c100 = 0;
  std::uint8_t quality = 0;
  std::uint8_t state = 0;
  std::uint16_t fan_pm = 0;
  bool operator==(const Telemetry&) const = default;
};

struct Event {
  static constexpr std::uint8_t kType = constants::kUartTypeEvent;
  static constexpr std::string_view kName = "Event";
  std::uint16_t seq = 0;
  std::uint32_t uptime_ms = 0;
  std::uint8_t code = 0;
  std::uint8_t severity = 0;
  std::uint8_t from_state = 0;
  std::uint8_t to_state = 0;
  std::int16_t temp_c100 = 0;
  bool operator==(const Event&) const = default;
};

struct Heartbeat {
  static constexpr std::uint8_t kType = constants::kUartTypeHeartbeat;
  static constexpr std::string_view kName = "Heartbeat";
  std::uint16_t seq = 0;
  std::uint32_t uptime_ms = 0;
  bool operator==(const Heartbeat&) const = default;
};

struct State {
  static constexpr std::uint8_t kType = constants::kUartTypeState;
  static constexpr std::string_view kName = "State";
  std::uint16_t seq = 0;
  std::uint16_t fw_version = 0;
  std::uint32_t boot_id = 0;
  std::uint16_t period_ms = 0;
  bool operator==(const State&) const = default;
};

// DIAG（01→03、v0.2.2 #37）。u16 の項目は 0xFFFF で飽和する
struct Diag {
  static constexpr std::uint8_t kType = constants::kUartTypeDiag;
  static constexpr std::string_view kName = "Diag";
  std::uint16_t seq = 0;
  std::uint32_t uptime_ms = 0;
  std::uint8_t reset_reason = 0;
  std::uint16_t reset_count = 0;
  std::uint16_t rx_crc_errors = 0;
  std::uint16_t rx_frame_errors = 0;
  std::uint16_t max_loop_us = 0;
  std::uint16_t overruns = 0;
  bool operator==(const Diag&) const = default;
};

struct Ack {
  static constexpr std::uint8_t kType = constants::kUartTypeAck;
  static constexpr std::string_view kName = "Ack";
  std::uint16_t seq = 0;
  std::uint16_t cmd_ref = 0;
  std::uint8_t status = 0;
  std::uint8_t reason = 0;
  std::uint8_t limited = 0;
  std::uint16_t applied = 0;
  bool operator==(const Ack&) const = default;
};

// CMD。args は op ごとの値（kCmdArgs の形）。形が決まっていない op は raw_args に生のまま
struct Cmd {
  static constexpr std::uint8_t kType = constants::kUartTypeCmd;
  static constexpr std::string_view kName = "Cmd";
  std::uint16_t seq = 0;
  std::uint16_t cmd_ref = 0;
  std::uint8_t op = 0;
  std::uint16_t ttl_s = 0;
  std::vector<std::int32_t> args;
  Bytes raw_args;
  bool operator==(const Cmd&) const = default;
};

using Frame = std::variant<Telemetry, Event, Heartbeat, State, Diag, Ack, Cmd>;

// 破棄したフレーム（frames.Discarded）。raw は受け取ったバイト列（区切りなし）
struct Discarded {
  std::string reason;
  std::string detail;
  Bytes raw;
};

using Decoded = std::variant<Frame, Discarded>;

// "TELEMETRY" など（gen_golden.py の type、bridge の欠番の名前）
[[nodiscard]] std::string type_name_upper(const Frame& frame);
// "Telemetry" など（Python のクラス名）
[[nodiscard]] std::string_view class_name(const Frame& frame);
[[nodiscard]] std::uint8_t type_code(const Frame& frame);
[[nodiscard]] std::uint16_t frame_seq(const Frame& frame);

}  // namespace gw::core::uart
