#include "core/uart/FrameCodec.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>

namespace gw::core::uart {

namespace {

using constants::kUartCrcSize;
using constants::kUartHeaderSize;
using constants::kUartPayloadMax;

constexpr unsigned kByte = 8U;
constexpr unsigned kByteMask = 0xFFU;

// ---- リトルエンディアンの書き込み・読み込み
class Writer {
 public:
  void u8(std::uint8_t v) { out_.push_back(v); }
  void u16(std::uint16_t v) {
    out_.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out_.push_back(static_cast<std::uint8_t>((v >> kByte) & kByteMask));
  }
  void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
  void u32(std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
      out_.push_back(static_cast<std::uint8_t>((v >> (kByte * i)) & kByteMask));
    }
  }
  void bytes(std::span<const std::uint8_t> data) {
    out_.insert(out_.end(), data.begin(), data.end());
  }
  [[nodiscard]] Bytes take() { return std::move(out_); }

 private:
  Bytes out_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
  std::uint8_t u8() { return data_[pos_++]; }
  std::uint16_t u16() {
    const auto lo = static_cast<unsigned>(data_[pos_]);
    const auto hi = static_cast<unsigned>(data_[pos_ + 1]);
    pos_ += 2;
    return static_cast<std::uint16_t>(lo | (hi << kByte));
  }
  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::uint32_t u32() {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (kByte * i);
    }
    pos_ += 4;
    return v;
  }
  [[nodiscard]] std::span<const std::uint8_t> rest() const { return data_.subspan(pos_); }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
};

// ペイロードの大きさ（固定長の種別）
constexpr std::size_t kTelemetrySize = 4 + 2 + 1 + 1 + 2;
constexpr std::size_t kEventSize = 4 + 1 + 1 + 1 + 1 + 2;
constexpr std::size_t kHeartbeatSize = 4;
constexpr std::size_t kStateSize = 2 + 4 + 2;
constexpr std::size_t kAckSize = 2 + 1 + 1 + 1 + 2;
constexpr std::size_t kCmdHeadSize = 2 + 1 + 2;
static_assert(constants::kUartDiagPayloadSize == 4 + 1 + 2 * 5);

std::size_t args_size(const CmdArgsSpec& spec) {
  return spec.types.size() * 2;
}

void write_args(Writer& w, const Cmd& cmd) {
  const auto* spec = cmd_args_spec(cmd.op);
  if (spec == nullptr) {
    if (!cmd.args.empty()) {
      throw std::invalid_argument("op の args の形は未定義（raw_args を使う）");
    }
    w.bytes(cmd.raw_args);
    return;
  }
  if (!cmd.raw_args.empty()) {
    throw std::invalid_argument("args の形が決まっている op に raw_args は使えない");
  }
  if (cmd.args.size() != spec->types.size()) {
    throw std::invalid_argument("args の数が op の形と合わない");
  }
  for (std::size_t i = 0; i < cmd.args.size(); ++i) {
    const auto value = cmd.args[i];
    if (spec->types[i] == ArgType::kU16) {
      if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("フィールドの値が範囲外: " + std::to_string(value));
      }
      w.u16(static_cast<std::uint16_t>(value));
    } else {
      if (value < std::numeric_limits<std::int16_t>::min() ||
          value > std::numeric_limits<std::int16_t>::max()) {
        throw std::invalid_argument("フィールドの値が範囲外: " + std::to_string(value));
      }
      w.i16(static_cast<std::int16_t>(value));
    }
  }
}

Bytes payload_of(const Frame& frame) {
  Writer w;
  std::visit(
      [&](const auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, Telemetry>) {
          w.u32(f.uptime_ms);
          w.i16(f.temp_c100);
          w.u8(f.quality);
          w.u8(f.state);
          w.u16(f.fan_pm);
        } else if constexpr (std::is_same_v<T, Event>) {
          w.u32(f.uptime_ms);
          w.u8(f.code);
          w.u8(f.severity);
          w.u8(f.from_state);
          w.u8(f.to_state);
          w.i16(f.temp_c100);
        } else if constexpr (std::is_same_v<T, Heartbeat>) {
          w.u32(f.uptime_ms);
        } else if constexpr (std::is_same_v<T, State>) {
          w.u16(f.fw_version);
          w.u32(f.boot_id);
          w.u16(f.period_ms);
        } else if constexpr (std::is_same_v<T, Diag>) {
          w.u32(f.uptime_ms);
          w.u8(f.reset_reason);
          w.u16(f.reset_count);
          w.u16(f.rx_crc_errors);
          w.u16(f.rx_frame_errors);
          w.u16(f.max_loop_us);
          w.u16(f.overruns);
        } else if constexpr (std::is_same_v<T, Ack>) {
          w.u16(f.cmd_ref);
          w.u8(f.status);
          w.u8(f.reason);
          w.u8(f.limited);
          w.u16(f.applied);
        } else {
          static_assert(std::is_same_v<T, Cmd>);
          w.u16(f.cmd_ref);
          w.u8(f.op);
          w.u16(f.ttl_s);
          write_args(w, f);
        }
      },
      frame);
  return w.take();
}

Discarded discard(std::string reason, std::string detail, std::span<const std::uint8_t> raw) {
  return Discarded{std::move(reason), std::move(detail), Bytes(raw.begin(), raw.end())};
}

Decoded decode_cmd(std::uint16_t seq, std::span<const std::uint8_t> payload,
                   std::span<const std::uint8_t> raw) {
  if (payload.size() < kCmdHeadSize) {
    return discard("payload_size", "CMD に " + std::to_string(payload.size()) + " バイト", raw);
  }
  Reader r(payload);
  Cmd cmd;
  cmd.seq = seq;
  cmd.cmd_ref = r.u16();
  cmd.op = r.u8();
  cmd.ttl_s = r.u16();
  const auto rest = r.rest();
  const auto* spec = cmd_args_spec(cmd.op);
  if (spec == nullptr) {
    cmd.raw_args.assign(rest.begin(), rest.end());
    return Frame{std::move(cmd)};
  }
  if (rest.size() != args_size(*spec)) {
    return discard("args_size", "op の args に " + std::to_string(rest.size()) + " バイト", raw);
  }
  Reader ar(rest);
  for (const auto type : spec->types) {
    cmd.args.push_back(type == ArgType::kU16 ? static_cast<std::int32_t>(ar.u16())
                                             : static_cast<std::int32_t>(ar.i16()));
  }
  return Frame{std::move(cmd)};
}

std::optional<std::size_t> fixed_size(std::uint8_t type) {
  switch (type) {
    case constants::kUartTypeTelemetry:
      return kTelemetrySize;
    case constants::kUartTypeEvent:
      return kEventSize;
    case constants::kUartTypeHeartbeat:
      return kHeartbeatSize;
    case constants::kUartTypeState:
      return kStateSize;
    case constants::kUartTypeDiag:
      return constants::kUartDiagPayloadSize;
    case constants::kUartTypeAck:
      return kAckSize;
    default:
      return std::nullopt;
  }
}

Frame decode_fixed(std::uint8_t type, std::uint16_t seq, Reader r) {
  switch (type) {
    case constants::kUartTypeTelemetry: {
      Telemetry f{seq, r.u32(), 0, 0, 0, 0};
      f.temp_c100 = r.i16();
      f.quality = r.u8();
      f.state = r.u8();
      f.fan_pm = r.u16();
      return f;
    }
    case constants::kUartTypeEvent: {
      Event f{seq, r.u32(), 0, 0, 0, 0, 0};
      f.code = r.u8();
      f.severity = r.u8();
      f.from_state = r.u8();
      f.to_state = r.u8();
      f.temp_c100 = r.i16();
      return f;
    }
    case constants::kUartTypeHeartbeat:
      return Heartbeat{seq, r.u32()};
    case constants::kUartTypeState: {
      State f{seq, r.u16(), 0, 0};
      f.boot_id = r.u32();
      f.period_ms = r.u16();
      return f;
    }
    case constants::kUartTypeDiag: {
      Diag f{seq, r.u32(), 0, 0, 0, 0, 0, 0};
      f.reset_reason = r.u8();
      f.reset_count = r.u16();
      f.rx_crc_errors = r.u16();
      f.rx_frame_errors = r.u16();
      f.max_loop_us = r.u16();
      f.overruns = r.u16();
      return f;
    }
    default: {
      Ack f{seq, r.u16(), 0, 0, 0, 0};
      f.status = r.u8();
      f.reason = r.u8();
      f.limited = r.u8();
      f.applied = r.u16();
      return f;
    }
  }
}

std::string hex2(unsigned value, int width) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out(static_cast<std::size_t>(width), '0');
  for (int i = width - 1; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kDigits[value & 0xFU];
    value >>= 4U;
  }
  return out;
}

}  // namespace

const std::vector<CmdArgsSpec>& cmd_args_specs() {
  static const std::vector<CmdArgsSpec> specs{
      {constants::kUartOpSetFanOverride, {ArgType::kU16}, {"duty_pm"}},
      {constants::kUartOpClearOverride, {}, {}},
      {constants::kUartOpSetThreshold,
       {ArgType::kI16, ArgType::kI16, ArgType::kI16},
       {"warn_c100", "crit_c100", "hyst_c100"}},
      {constants::kUartOpGetStatus, {}, {}},
      {constants::kUartOpGetDiag, {}, {}},
  };
  return specs;
}

const CmdArgsSpec* cmd_args_spec(std::uint8_t op) {
  const auto& specs = cmd_args_specs();
  const auto it =
      std::find_if(specs.begin(), specs.end(), [&](const auto& s) { return s.op == op; });
  return it == specs.end() ? nullptr : &*it;
}

Bytes encode_frame(const Frame& frame) {
  const Bytes payload = payload_of(frame);
  if (payload.size() > kUartPayloadMax) {
    throw std::invalid_argument("ペイロード長 " + std::to_string(payload.size()) + " が上限 " +
                                std::to_string(kUartPayloadMax) + " を超える");
  }
  Writer body;
  body.u8(type_code(frame));
  body.u16(frame_seq(frame));
  body.u8(static_cast<std::uint8_t>(payload.size()));
  body.bytes(payload);
  Bytes raw = body.take();
  const auto crc = crc16_ccitt_false(raw);
  raw.push_back(static_cast<std::uint8_t>(crc & kByteMask));
  raw.push_back(static_cast<std::uint8_t>((crc >> kByte) & kByteMask));
  Bytes out = cobs_encode(raw);
  out.push_back(constants::kUartFrameDelimiter);
  return out;
}

Decoded decode_frame(std::span<const std::uint8_t> data) {
  const auto encoded = (!data.empty() && data.back() == constants::kUartFrameDelimiter)
                           ? data.first(data.size() - 1)
                           : data;
  auto decoded = cobs_decode(encoded);
  if (!decoded.data) {
    return discard("cobs_invalid", decoded.error, encoded);
  }
  const Bytes& raw = *decoded.data;
  if (raw.size() < kUartHeaderSize + kUartCrcSize) {
    return discard("too_short", std::to_string(raw.size()) + " バイト", raw);
  }
  const std::span<const std::uint8_t> all(raw);
  const auto body = all.first(raw.size() - kUartCrcSize);
  const auto received_crc = static_cast<std::uint16_t>(
      static_cast<unsigned>(raw[raw.size() - 2]) | (static_cast<unsigned>(raw.back()) << kByte));
  const auto computed = crc16_ccitt_false(body);
  if (received_crc != computed) {
    return discard("crc_mismatch",
                   "受信 0x" + hex2(received_crc, 4) + " 計算 0x" + hex2(computed, 4), raw);
  }
  Reader header(body);
  const auto type = header.u8();
  const auto seq = header.u16();
  const auto length = header.u8();
  const auto payload = body.subspan(kUartHeaderSize);
  if (length != payload.size() || length > kUartPayloadMax) {
    return discard("length_mismatch",
                   "長さ " + std::to_string(length) + "、実際 " + std::to_string(payload.size()),
                   raw);
  }
  if (type == constants::kUartTypeCmd) {
    return decode_cmd(seq, payload, raw);
  }
  const auto size = fixed_size(type);
  if (!size) {
    return discard("unknown_type", "種別 0x" + hex2(type, 2), raw);
  }
  if (payload.size() != *size) {
    return discard("payload_size", std::to_string(payload.size()) + " バイト", raw);
  }
  return decode_fixed(type, seq, Reader(payload));
}

std::vector<Decoded> FrameReader::feed(std::span<const std::uint8_t> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  std::vector<Decoded> out;
  auto start = buffer_.begin();
  for (;;) {
    const auto delim = std::find(start, buffer_.end(), constants::kUartFrameDelimiter);
    if (delim == buffer_.end()) {
      break;
    }
    if (delim != start) {
      out.push_back(decode_frame(
          std::span<const std::uint8_t>(&*start, static_cast<std::size_t>(delim - start))));
    }
    start = delim + 1;
  }
  buffer_.erase(buffer_.begin(), start);
  return out;
}

std::string type_name_upper(const Frame& frame) {
  std::string name{class_name(frame)};
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return name;
}

std::string_view class_name(const Frame& frame) {
  return std::visit([](const auto& f) { return std::decay_t<decltype(f)>::kName; }, frame);
}

std::uint8_t type_code(const Frame& frame) {
  return std::visit([](const auto& f) { return std::decay_t<decltype(f)>::kType; }, frame);
}

std::uint16_t frame_seq(const Frame& frame) {
  return std::visit([](const auto& f) { return f.seq; }, frame);
}

}  // namespace gw::core::uart
