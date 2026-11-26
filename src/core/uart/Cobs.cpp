#include "core/uart/Cobs.h"

#include <algorithm>

#include "core/common/Constants.h"

namespace gw::core::uart {

namespace {
constexpr std::size_t kMaxBlock = 254;
}  // namespace

Bytes cobs_encode(std::span<const std::uint8_t> data) {
  Bytes out;
  out.reserve(data.size() + data.size() / kMaxBlock + 2);
  Bytes block;
  bool ended_with_full_block = false;
  for (const auto byte : data) {
    ended_with_full_block = false;
    if (byte == 0) {
      out.push_back(static_cast<std::uint8_t>(block.size() + 1));
      out.insert(out.end(), block.begin(), block.end());
      block.clear();
    } else {
      block.push_back(byte);
      if (block.size() == kMaxBlock) {
        out.push_back(static_cast<std::uint8_t>(kMaxBlock + 1));
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
        ended_with_full_block = true;
      }
    }
  }
  if (!ended_with_full_block) {
    out.push_back(static_cast<std::uint8_t>(block.size() + 1));
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

CobsResult cobs_decode(std::span<const std::uint8_t> data) {
  if (data.empty()) {
    return {std::nullopt, "空のデータ"};
  }
  Bytes out;
  out.reserve(data.size());
  std::size_t i = 0;
  const std::size_t n = data.size();
  while (i < n) {
    const std::uint8_t code = data[i];
    if (code == 0) {
      return {std::nullopt, "位置 " + std::to_string(i) + " に 0x00 がある"};
    }
    ++i;
    const std::size_t end = i + code - 1;
    if (end > n) {
      return {std::nullopt, "位置 " + std::to_string(i - 1) + " の符号 " + std::to_string(code) +
                                " がデータの末尾を超える"};
    }
    const auto block = data.subspan(i, end - i);
    if (std::find(block.begin(), block.end(), std::uint8_t{0}) != block.end()) {
      return {std::nullopt, "ブロックの中に 0x00 がある"};
    }
    out.insert(out.end(), block.begin(), block.end());
    i = end;
    if (code <= kMaxBlock && i < n) {
      out.push_back(0);
    }
  }
  return {std::move(out), {}};
}

std::uint16_t crc16_ccitt_false(std::span<const std::uint8_t> data) {
  constexpr unsigned kTopBit = 0x8000U;
  constexpr unsigned kMask = 0xFFFFU;
  unsigned crc = constants::kCrc16Init;
  for (const auto byte : data) {
    crc ^= static_cast<unsigned>(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & kTopBit) != 0 ? ((crc << 1U) ^ constants::kCrc16Poly) : (crc << 1U);
      crc &= kMask;
    }
  }
  return static_cast<std::uint16_t>(crc ^ constants::kCrc16XorOut);
}

}  // namespace gw::core::uart
