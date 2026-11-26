// COBS（区切りは 0x00）と CRC-16/CCITT-FALSE（第7章「フレーム構造」）。
// 対応する Python：iotspec.uart.cobs（encode・decode）、iotspec.uart.crc（crc16_ccitt_false）
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gw::core::uart {

using Bytes = std::vector<std::uint8_t>;

// 符号化。結果に 0x00 は含まれない（区切りは FrameCodec が付ける）。
// 254 バイトの非ゼロの並びで終わるときは末尾に 0x01 を付けない（cobs.py と同じ）
[[nodiscard]] Bytes cobs_encode(std::span<const std::uint8_t> data);

struct CobsResult {
  std::optional<Bytes> data;
  std::string error;  // data が無いときの理由
};
// 復号。0x00 を含む・符号がデータの末尾を超える・空なら error
[[nodiscard]] CobsResult cobs_decode(std::span<const std::uint8_t> data);

[[nodiscard]] std::uint16_t crc16_ccitt_false(std::span<const std::uint8_t> data);

}  // namespace gw::core::uart
