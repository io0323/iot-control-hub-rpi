// フレームの符号化・復号（iotspec.uart.frames の encode_frame・decode_frame・FrameReader）。
//   [種別 u8][seq u16][長さ u8][ペイロード 0–200B][CRC-16/CCITT-FALSE u16]
// をリトルエンディアンで並べ、COBS で符号化して末尾に 0x00 を付ける。CRC
// は種別からペイロード末尾まで。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/uart/Frames.h"

namespace gw::core::uart {

enum class ArgType : std::uint8_t { kU16, kI16 };

struct CmdArgsSpec {
  std::uint8_t op;
  std::vector<ArgType> types;
  std::vector<std::string_view> names;
};

// CMD の args の形（v0.2.1 #20、frames.CMD_ARGS_FORMAT と CMD_ARG_NAMES）
[[nodiscard]] const std::vector<CmdArgsSpec>& cmd_args_specs();
[[nodiscard]] const CmdArgsSpec* cmd_args_spec(std::uint8_t op);

// 符号化（末尾の 0x00 を含む）。値が範囲外・ペイロードが上限超なら std::invalid_argument
[[nodiscard]] Bytes encode_frame(const Frame& frame);

// 復号（末尾の 0x00 はあってもなくてもよい）。例外は投げず、壊れたものは Discarded
[[nodiscard]] Decoded decode_frame(std::span<const std::uint8_t> data);

// バイト列を少しずつ受け取り、0x00 で区切ってフレームを取り出す（FrameReader）
class FrameReader {
 public:
  // data を足し、そろったフレームを順に返す。空のフレーム（0x00 の連続）は読み飛ばす
  [[nodiscard]] std::vector<Decoded> feed(std::span<const std::uint8_t> data);
  [[nodiscard]] std::size_t buffered() const { return buffer_.size(); }
  void clear() { buffer_.clear(); }

 private:
  Bytes buffer_;
};

}  // namespace gw::core::uart
