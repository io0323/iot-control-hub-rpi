#include "core/uart/UartBridge.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "core/common/Constants.h"
#include "core/common/Topics.h"
#include "core/uart/FrameCodec.h"
#include "core/uart/Mapping.h"

namespace gw::core::uart {

namespace {

// 第7章「種別と対応」：TELEMETRY は temp_board と fan_main に対応する
constexpr std::string_view kTelemetryChannel = "temp_board";
constexpr std::string_view kTelemetryUnit = "degC";
constexpr std::string_view kTelemetryActuator = "fan_main";
constexpr double kTempScale = 100.0;       // temp_c100 ÷ 100 = degC（`* 0.01` は使わない）
constexpr std::uint16_t kNoLeaseTtlS = 0;  // リースを持たない op の ttl_s（#21）
constexpr std::string_view kLeasedOp = "set_actuator";
constexpr int kLevelMax = 1000;  // common.schema.json の level の maximum

bool is_applied_status(std::string_view status) {
  // cmd/ack に applied を付ける status（v0.2.2 #44：bridge.APPLIED_STATUSES）
  return status == "accepted" || status == "done";
}

std::string op_name(std::uint8_t op) {
  for (const auto& [code, name] : constants::kUartOpNames) {
    if (code == op) {
      return std::string{name};
    }
  }
  static constexpr std::string_view kDigits = "0123456789abcdef";
  return std::string{"0x"} + kDigits[(op >> 4U) & 0xFU] + kDigits[op & 0xFU];
}

// Python の int(value)。変換できなければ nullopt（TypeError・ValueError）
std::optional<std::int64_t> python_int(const Json& value) {
  if (value.is_boolean()) {
    return value.get<bool>() ? 1 : 0;
  }
  if (value.is_number_integer()) {
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return value.get<std::int64_t>();
  }
  if (value.is_number_float()) {
    const double d = value.get<double>();
    if (!std::isfinite(d) || std::fabs(d) >= 9.2e18) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(std::trunc(d));
  }
  if (value.is_string()) {
    auto text = value.get<std::string>();
    const auto first = text.find_first_not_of(" \t\n\r\f\v");
    const auto last = text.find_last_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) {
      return std::nullopt;
    }
    text = text.substr(first, last - first + 1);
    const char* begin = text.data();
    if (*begin == '+') {
      ++begin;
    }
    std::int64_t out = 0;
    const auto [ptr, ec] = std::from_chars(begin, text.data() + text.size(), out);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return out;
  }
  return std::nullopt;
}

bool truthy(const Json& value) {
  if (value.is_null()) {
    return false;
  }
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number()) {
    return value.get<double>() != 0.0;
  }
  if (value.is_string() || value.is_object() || value.is_array()) {
    return !value.empty();
  }
  return true;
}

// iotspec.lease.resolve_lease_s。set_actuator 以外は nullopt。不正なら CommandRejected
std::variant<std::optional<int>, CommandRejected> resolve_lease_s(const Json& cmd) {
  if (!cmd.contains("op") || cmd["op"] != kLeasedOp) {
    return std::optional<int>{};
  }
  const Json value = cmd.contains("lease_s") ? cmd["lease_s"] : Json(constants::kLeaseDefaultS);
  if (value.is_boolean() || !value.is_number_integer()) {
    return CommandRejected{"invalid_arg", "lease_s が整数ではない: " + value.dump()};
  }
  const auto lease = value.get<std::int64_t>();
  if (lease < constants::kLeaseMinS || lease > constants::kLeaseMaxS) {
    return CommandRejected{"invalid_arg", "lease_s が範囲外（1–600）: " + std::to_string(lease)};
  }
  return std::optional<int>{static_cast<int>(lease)};
}

// _emit の JSON Schema 検証のうち、UART の値から起こりうる違反（open-questions #9）
std::optional<std::string> schema_violation(const Envelope& env) {
  const auto& p = env.payload;
  if (env.schema == "iot.v1.telemetry" && p.contains("actuators")) {
    for (const auto& a : p["actuators"]) {
      const auto level = a.value("level", 0);
      if (level < 0 || level > kLevelMax) {
        return "/payload/actuators/0/level: " + std::to_string(level) +
               " is greater than the maximum of 1000";
      }
    }
  }
  if (env.schema == "iot.v1.cmd_ack") {
    const auto status = p.value("status", "");
    if ((status == "rejected" || status == "failed") && p["reason"].is_null()) {
      return "/payload/reason: None is not one of the cmd_reason values";
    }
  }
  return std::nullopt;
}

}  // namespace

std::string format_fw_version(std::uint16_t fw_version) {
  constexpr unsigned kShift = 8U;
  constexpr unsigned kLow = 0xFFU;
  return std::to_string(fw_version >> kShift) + "." + std::to_string(fw_version & kLow);
}

UartBridge::UartBridge(Profile profile, IIdGenerator& ids, std::optional<std::string> bridged_by)
    : profile_(std::move(profile)),
      ids_(ids),
      bridged_by_(std::move(bridged_by)),
      mode_(constants::kUartInitialDeviceState) {
  if (profile_.transport != "uart") {
    throw std::invalid_argument("UART のプロファイルではない: " + profile_.device_id);
  }
}

// ------------------------------------------------------------ 受信（01 → 03）

BridgeResult UartBridge::receive(const Decoded& decoded, SystemTime at) {
  if (const auto* bad = std::get_if<Discarded>(&decoded)) {
    return discard(bad->reason, bad->detail);
  }
  return receive(std::get<Frame>(decoded), at);
}

BridgeResult UartBridge::receive(const Frame& frame, SystemTime at) {
  if (std::holds_alternative<Cmd>(frame)) {
    return discard("unexpected_direction", "CMD は 03 → 01 の向き");
  }
  if (const auto* state = std::get_if<State>(&frame)) {
    return on_state(*state, at);
  }

  bool request = false;
  const auto uptime = std::visit(
      [](const auto& f) -> std::optional<std::uint32_t> {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, Telemetry> || std::is_same_v<T, Event> ||
                      std::is_same_v<T, Heartbeat> || std::is_same_v<T, Diag>) {
          return f.uptime_ms;
        } else {
          return std::nullopt;
        }
      },
      frame);
  if (uptime) {
    if (last_uptime_ && *uptime < *last_uptime_) {
      awaiting_state_ = true;
      reboot_detected_ = true;
      state_requested_ = false;
    }
    last_uptime_ = uptime;
  }
  if (awaiting_state_ && !state_requested_) {
    state_requested_ = true;
    request = true;
  }
  const auto seq = extend(type_code(frame), type_name_upper(frame), frame_seq(frame));

  if (std::holds_alternative<Heartbeat>(frame)) {
    BridgeResult result;
    result.request_state = request;
    return result;
  }
  if (awaiting_state_) {
    if (std::holds_alternative<Telemetry>(frame)) {
      ++dropped_telemetry_;
    }
    return discard("awaiting_state", std::string{class_name(frame)}, request);
  }
  if (const auto* d = std::get_if<Diag>(&frame)) {
    auto result = discard(std::string{kNotConverted}, "DIAG", request);
    result.diag = diag(*d, at);
    return result;
  }
  if (const auto* a = std::get_if<Ack>(&frame);
      a != nullptr && internal_refs_.contains(a->cmd_ref)) {
    return internal_ack(*a, request);
  }
  std::optional<Envelope> env;
  try {
    if (const auto* t = std::get_if<Telemetry>(&frame)) {
      env = telemetry(*t, seq, at);
    } else if (const auto* e = std::get_if<Event>(&frame)) {
      env = event(*e, seq, at);
    } else {
      const auto& a = std::get<Ack>(frame);
      const auto it = cmd_table_.find(a.cmd_ref);
      if (it == cmd_table_.end()) {
        return discard("unknown_cmd_ref", "cmd_ref " + std::to_string(a.cmd_ref), request);
      }
      env = ack(a, seq, it->second.first, it->second.second, at);
    }
  } catch (const UnknownValueError& exc) {
    return discard("unknown_value", exc.what(), request);
  }
  return emit(std::move(*env), request);
}

BridgeResult UartBridge::on_state(const State& frame, SystemTime at) {
  const auto boot_id = format_boot_id(frame.boot_id);
  if (boot_id != boot_id_ || reboot_detected_) {
    rx_.clear();
    rx_last_.clear();
    mode_ = constants::kUartInitialDeviceState;
  }
  boot_id_ = boot_id;
  awaiting_state_ = false;
  reboot_detected_ = false;
  state_requested_ = false;
  last_uptime_.reset();
  const auto seq = extend(State::kType, "STATE", frame.seq);
  period_ms_ = frame.period_ms;
  fw_version_ = format_fw_version(frame.fw_version);
  return emit(envelope(kKindState, seq, state_payload("online"), std::nullopt, at), false);
}

Json UartBridge::state_payload(const std::string& status) const {
  Json payload = Json::object();
  payload["status"] = status;
  payload["mode"] = mode_;
  Json channels = Json::array();
  for (const auto& c : profile_.channels) {
    Json ch = Json::object();
    ch["channel"] = c.channel;
    ch["unit"] = c.unit;
    // STATE の period_ms を優先する（0 ならプロファイルの値。Python の `or`）
    ch["period_ms"] =
        (period_ms_ && *period_ms_ != 0) ? static_cast<std::int64_t>(*period_ms_) : c.period_ms;
    channels.push_back(std::move(ch));
  }
  payload["channels"] = std::move(channels);
  Json actuators = Json::array();
  for (const auto& a : profile_.actuators) {
    Json act = Json::object();
    act["id"] = a.id;
    act["min"] = a.min;
    act["max"] = a.max;
    act["max_on_s"] = a.max_on_s;
    actuators.push_back(std::move(act));
  }
  payload["actuators"] = std::move(actuators);
  payload["fw_version"] = fw_version_;
  if (bridged_by_) {
    payload["bridged_by"] = *bridged_by_;
  }
  return payload;
}

std::optional<Envelope> UartBridge::offline_state(SystemTime at) {
  if (!boot_id_) {
    return std::nullopt;
  }
  const auto it = rx_last_.find(State::kType);
  const std::int64_t last = it == rx_last_.end() ? -1 : static_cast<std::int64_t>(it->second);
  const auto seq =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(last + 1) & constants::kUint32Max);
  return envelope(kKindState, seq, state_payload("offline"), std::nullopt, at);
}

Envelope UartBridge::telemetry(const Telemetry& frame, std::uint32_t seq, SystemTime at) {
  const auto device_state = profile_.state_name(frame.state);
  Json reading = Json::object();
  reading["channel"] = kTelemetryChannel;
  reading["value"] = static_cast<double>(frame.temp_c100) / kTempScale;
  reading["unit"] = kTelemetryUnit;
  reading["quality"] = to_common_string(quality_table(), frame.quality);
  Json actuator = Json::object();
  actuator["id"] = kTelemetryActuator;
  actuator["level"] = frame.fan_pm;
  Json payload = Json::object();
  payload["readings"] = Json::array({std::move(reading)});
  payload["device_state"] = device_state;
  payload["actuators"] = Json::array({std::move(actuator)});
  mode_ = device_state;
  return envelope(kKindTelemetry, seq, std::move(payload), frame.uptime_ms, at);
}

Envelope UartBridge::event(const Event& frame, std::uint32_t seq, SystemTime at) {
  static const int kBootCode = event_code_table().to_uart("BOOT");
  Json detail = Json::object();
  if (frame.code == kBootCode) {
    // BOOT に限り from は reset_reason、to は 0（v0.2.2 #38）
    if (frame.to_state != constants::kUartBootEventTo) {
      throw UnknownValueError("BOOT の to が 0 でない: " + std::to_string(frame.to_state));
    }
    detail["reset_reason"] = to_common_string(reset_reason_table(), frame.from_state);
  } else {
    detail["from_state"] = profile_.state_name(frame.from_state);
    detail["to_state"] = profile_.state_name(frame.to_state);
  }
  Json payload = Json::object();
  payload["code"] = to_common_string(event_code_table(), frame.code);
  payload["severity"] = to_common_string(severity_table(), frame.severity);
  payload["channel"] = kTelemetryChannel;
  payload["value"] = static_cast<double>(frame.temp_c100) / kTempScale;
  payload["detail"] = std::move(detail);
  return envelope(kKindEvent, seq, std::move(payload), frame.uptime_ms, at);
}

Json UartBridge::diag(const Diag& frame, SystemTime at) const {
  // DIAG の項目（復号した値そのまま）に受信時刻と boot_id を加える（#37）
  Json out = Json::object();
  out["received_at"] = format_ts(at);
  out["boot_id"] = boot_id_ ? Json(*boot_id_) : Json(nullptr);
  out["uptime_ms"] = frame.uptime_ms;
  out["reset_reason"] = frame.reset_reason;
  out["reset_count"] = frame.reset_count;
  out["rx_crc_errors"] = frame.rx_crc_errors;
  out["rx_frame_errors"] = frame.rx_frame_errors;
  out["max_loop_us"] = frame.max_loop_us;
  out["overruns"] = frame.overruns;
  return out;
}

BridgeResult UartBridge::internal_ack(const Ack& frame, bool request) {
  const auto op = internal_refs_.at(frame.cmd_ref);
  Json ack_json = Json::object();
  try {
    ack_json["cmd_ref"] = frame.cmd_ref;
    ack_json["op"] = op_name(op);
    ack_json["status"] = to_common_string(status_table(), frame.status);
    const auto reason = reason_table().to_common(frame.reason);
    ack_json["reason"] = reason ? Json(std::string{*reason}) : Json(nullptr);
    ack_json["limited"] = frame.limited;
    ack_json["applied"] = frame.applied;
  } catch (const UnknownValueError& exc) {
    return discard("unknown_value", exc.what(), request);
  }
  auto result =
      discard(std::string{kNotConverted}, ack_json["op"].get<std::string>() + " の ACK", request);
  result.internal_ack = std::move(ack_json);
  return result;
}

Envelope UartBridge::ack(const Ack& frame, std::uint32_t seq, const std::string& cmd_id,
                         std::uint8_t uart_op, SystemTime at) {
  if (frame.limited > 1) {
    throw UnknownValueError("limited: 0/1 以外の値 " + std::to_string(frame.limited));
  }
  Json payload = Json::object();
  payload["cmd_id"] = cmd_id;
  payload["status"] = to_common_string(status_table(), frame.status);
  payload["limited"] = frame.limited == 1;
  const auto reason = reason_table().to_common(frame.reason);
  payload["reason"] = reason ? Json(std::string{*reason}) : Json(nullptr);
  // applied は出力を扱う op で、指令を受け付けた・実行したときだけ付ける（#19、v0.2.2 #44）
  if (uart_op == constants::kUartOpSetFanOverride &&
      is_applied_status(payload["status"].get<std::string>())) {
    Json applied = Json::object();
    applied["level"] = frame.applied;
    payload["applied"] = std::move(applied);
  }
  return envelope(kKindCmdAck, seq, std::move(payload), std::nullopt, at);
}

Envelope UartBridge::envelope(std::string_view kind, std::uint32_t seq, Json payload,
                              std::optional<std::uint32_t> uptime_ms, SystemTime at) {
  const auto info = require_kind(kind);
  Envelope env;
  env.schema = std::string{info.schema};
  env.msg_id = ids_.next_ulid();
  env.device_id = profile_.device_id;
  env.seq = seq;
  env.boot_id = boot_id_.value_or("");
  env.ts = at;
  env.ts_source = "gateway";
  env.payload = std::move(payload);
  env.uptime_ms = uptime_ms;
  return env;
}

std::uint32_t UartBridge::extend(std::uint8_t frame_type, const std::string& name,
                                 std::uint16_t seq16) {
  const auto extended = rx_[frame_type].extend(seq16);
  const auto it = rx_last_.find(frame_type);
  if (it != rx_last_.end() &&
      static_cast<std::uint64_t>(extended) > static_cast<std::uint64_t>(it->second) + 1) {
    uart_gaps_[name] += extended - it->second - 1;
  }
  rx_last_[frame_type] = extended;
  return extended;
}

BridgeResult UartBridge::emit(Envelope env, bool request) {
  if (const auto violation = schema_violation(env)) {
    return discard("schema_invalid", *violation, request);
  }
  BridgeResult result;
  result.envelope = std::move(env);
  result.request_state = request;
  return result;
}

BridgeResult UartBridge::discard(const std::string& reason, std::string detail, bool request) {
  ++discards_[reason];
  BridgeResult result;
  result.discarded = reason;
  result.detail = std::move(detail);
  result.request_state = request;
  return result;
}

// ------------------------------------------------------------ 送信（03 → 01）

std::uint16_t UartBridge::next_tx_seq(std::uint8_t frame_type) {
  auto& seq = tx_seq_[frame_type];
  const auto current = seq;
  seq = static_cast<std::uint16_t>((current + 1U) % constants::kUartSeqModulo);
  return current;
}

std::variant<Cmd, CommandRejected> UartBridge::command_to_frame(const Json& cmd,
                                                                std::uint16_t cmd_ref) {
  const Json op = cmd.contains("op") ? cmd["op"] : Json(nullptr);
  const Json args = (cmd.contains("args") && truthy(cmd["args"])) ? cmd["args"] : Json::object();
  std::optional<std::string> target;
  bool target_is_other = false;  // id が文字列でない（どの target とも一致しない）
  if (args.is_object() && args.contains("id")) {
    if (args["id"].is_string()) {
      target = args["id"].get<std::string>();
    } else if (!args["id"].is_null()) {
      target_is_other = true;
    }
  }
  const CommandSpec* spec = nullptr;
  if (op.is_string()) {
    for (const auto& candidate : profile_.commands) {
      if (candidate.op == op.get<std::string>() &&
          (!candidate.target || (!target_is_other && candidate.target == target))) {
        spec = &candidate;
        break;
      }
    }
  }
  if (spec == nullptr) {
    const bool known_op = op.is_string() && profile_.has_op(op.get<std::string>());
    return CommandRejected{known_op ? "unknown_target" : "unknown_op",
                           "op=" + op.dump() + " target=" + (target ? *target : "None")};
  }
  std::vector<std::int32_t> values;
  const auto* arg_spec = cmd_args_spec(spec->uart_op);
  if (arg_spec != nullptr) {
    for (const auto name : arg_spec->names) {
      const auto field = std::find_if(spec->args.begin(), spec->args.end(),
                                      [&](const auto& p) { return p.first == name; });
      if (field == spec->args.end() || !args.is_object() || !args.contains(field->second)) {
        return CommandRejected{"invalid_arg", "args: " + std::string{name}};
      }
      const auto value = python_int(args[field->second]);
      if (!value || *value < std::numeric_limits<std::int32_t>::min() ||
          *value > std::numeric_limits<std::int32_t>::max()) {
        return CommandRejected{"invalid_arg", "args: " + field->second};
      }
      values.push_back(static_cast<std::int32_t>(*value));
    }
  }
  std::uint16_t ttl_s = kNoLeaseTtlS;
  if (spec->ttl_from_lease) {
    auto lease = resolve_lease_s(cmd);
    if (auto* rejected = std::get_if<CommandRejected>(&lease)) {
      return *rejected;
    }
    const auto value = std::get<std::optional<int>>(lease);
    ttl_s = value ? static_cast<std::uint16_t>(*value) : kNoLeaseTtlS;
  }
  if (!cmd.contains("cmd_id")) {
    throw std::invalid_argument("cmd に cmd_id がない");
  }
  const auto& id = cmd["cmd_id"];
  Cmd frame;
  frame.seq = next_tx_seq(Cmd::kType);
  frame.cmd_ref = cmd_ref;
  frame.op = spec->uart_op;
  frame.ttl_s = ttl_s;
  frame.args = std::move(values);
  cmd_table_[cmd_ref] = {id.is_string() ? id.get<std::string>() : id.dump(), spec->uart_op};
  internal_refs_.erase(cmd_ref);
  return frame;
}

Cmd UartBridge::internal_frame(std::uint8_t op, std::uint16_t cmd_ref,
                               std::vector<std::int32_t> args) {
  const auto* spec = cmd_args_spec(op);
  if (op == constants::kUartOpSetFanOverride || spec == nullptr) {
    throw std::invalid_argument("内部の操作にできない op: " + op_name(op));
  }
  if (args.size() != spec->names.size()) {
    throw std::invalid_argument("op " + op_name(op) + " の args は " +
                                std::to_string(spec->names.size()) + " 個");
  }
  Cmd frame;
  frame.seq = next_tx_seq(Cmd::kType);
  frame.cmd_ref = cmd_ref;
  frame.op = op;
  frame.ttl_s = kNoLeaseTtlS;
  frame.args = std::move(args);
  internal_refs_[cmd_ref] = op;
  cmd_table_.erase(cmd_ref);
  return frame;
}

Heartbeat UartBridge::heartbeat_frame(std::uint32_t uptime_ms) {
  return Heartbeat{next_tx_seq(Heartbeat::kType), uptime_ms};
}

}  // namespace gw::core::uart
