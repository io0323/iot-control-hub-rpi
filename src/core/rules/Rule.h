// ルールの定義（基本設計 第6章「ルール定義」、FR-07・08）。rules.yaml を読んだ結果（adapters/config/
// RulesConfig が作る）で、core はこの形だけを扱う。
//
//   rules:
//     - id: cool-on-high-temp
//       priority: auto                 # safety | manual | auto | schedule
//       when: { any: [ { device: mcu-c-01, channel: temp_board, op: ">=", value: 45.0 } ] }
//       for: 10s                       # 条件が続いたら発火（省略は 0）
//       hysteresis: 2.0                # 解除の向きは op から決める（open-questions #31）
//       cooldown: 30s
//       then:
//         - { action: command, device: mcu-cpp-01, op: set_actuator,
//             args: { id: fan_main, level: 800 }, lease: 60s, hold: true }
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/command/Priority.h"
#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/device/DeviceRegistry.h"

namespace gw::core::rules {

enum class Op : std::uint8_t { kGe, kGt, kLe, kLt, kEq, kNe };
[[nodiscard]] std::optional<Op> parse_op(std::string_view text);
[[nodiscard]] std::string_view to_string(Op op);

inline constexpr std::string_view kAnyDevice = "*";

struct ChannelLeaf {
  std::string device;
  std::string channel;
  Op op = Op::kGe;
  double value = 0.0;
  bool operator==(const ChannelLeaf&) const = default;
};
struct StatusLeaf {
  std::string device;  // "*" はどれか1つ（open-questions #32）
  device::DeviceStatus status = device::DeviceStatus::kOffline;
  bool operator==(const StatusLeaf&) const = default;
};
struct ModeLeaf {
  std::string mode;  // NORMAL・ALERT・EMERGENCY
  bool operator==(const ModeLeaf&) const = default;
};
struct TimeLeaf {
  int from_min = 0;  // 0:00 からの分（現地時刻）
  int to_min = 0;    // from > to なら日をまたぐ
  bool operator==(const TimeLeaf&) const = default;
};

// all・any・not と葉
struct Condition {
  enum class Kind : std::uint8_t { kAll, kAny, kNot, kChannel, kStatus, kMode, kTime };
  Kind kind = Kind::kAll;
  std::vector<Condition> children;  // all・any・not（not は1つ）
  std::variant<std::monostate, ChannelLeaf, StatusLeaf, ModeLeaf, TimeLeaf> leaf;
  bool operator==(const Condition&) const = default;
};

struct CommandAction {
  std::string device;
  std::string op;
  Json args = Json::object();
  int lease_s = 0;    // set_actuator だけ（1〜600）
  bool hold = false;  // active の間リースを更新し、解除時に release を送る
  bool operator==(const CommandAction&) const = default;
};
struct ModeAction {
  std::string mode;  // ALERT・EMERGENCY（open-questions #35）
  bool operator==(const ModeAction&) const = default;
};
struct NotifyAction {
  std::string level = "info";  // RULE_FIRED の重大度（v0.2.3 #47）
  std::optional<std::string> message;
  bool operator==(const NotifyAction&) const = default;
};
struct ReconnectAction {
  bool operator==(const ReconnectAction&) const = default;
};
using Action = std::variant<CommandAction, ModeAction, NotifyAction, ReconnectAction>;

struct Rule {
  std::string id;
  command::Priority priority = command::Priority::kAuto;
  Condition when;
  Millis for_duration{0};
  Millis cooldown{0};
  std::optional<double> hysteresis;
  std::vector<Action> then;

  [[nodiscard]] std::string owner() const { return "rule:" + id; }
  // 再読込の前後で同じルールか（id と定義：when・for・hysteresis・then・priority・cooldown。#39）
  bool operator==(const Rule&) const = default;
  [[nodiscard]] const NotifyAction* notify() const;
  [[nodiscard]] std::optional<std::string> sets_mode() const;
};

struct RuleSet {
  std::vector<Rule> rules;
};

}  // namespace gw::core::rules
