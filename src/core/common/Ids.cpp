#include "core/common/Ids.h"

#include <array>
#include <cstdio>

namespace gw::core {

namespace {

constexpr std::string_view kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int kUlidChars = 26;
constexpr int kUlidBytes = 16;
constexpr int kBitsPerChar = 5;
constexpr int kTimeBytes = 6;
constexpr std::uint32_t kByteMask = 0xFF;

std::random_device::result_type seed() {
  std::random_device device;
  return device();
}

}  // namespace

std::string encode_ulid(std::uint64_t unix_ms, const UlidRandom& random) {
  // 128 ビット（時刻 48 ＋乱数 80）を上位から5ビットずつ。26文字×5＝130 なので先頭2ビットは0
  std::array<std::uint8_t, kUlidBytes> bytes{};
  for (int i = 0; i < kTimeBytes; ++i) {
    bytes.at(static_cast<std::size_t>(i)) =
        static_cast<std::uint8_t>((unix_ms >> (8 * (kTimeBytes - 1 - i))) & kByteMask);
  }
  for (std::size_t i = 0; i < random.size(); ++i) {
    bytes.at(kTimeBytes + i) = random.at(i);
  }
  std::string out(kUlidChars, '0');
  constexpr int kTotalBits = kUlidChars * kBitsPerChar;  // 130
  for (int c = 0; c < kUlidChars; ++c) {
    unsigned value = 0;
    for (int b = 0; b < kBitsPerChar; ++b) {
      const int bit = c * kBitsPerChar + b - (kTotalBits - kUlidBytes * 8);  // 先頭2ビットは0
      value <<= 1U;
      if (bit >= 0) {
        const auto byte = bytes.at(static_cast<std::size_t>(bit / 8));
        value |= (static_cast<unsigned>(byte) >> (7 - (bit % 8))) & 1U;
      }
    }
    out.at(static_cast<std::size_t>(c)) = kCrockford.at(value);
  }
  return out;
}

MonotonicUlidGenerator::MonotonicUlidGenerator(const IClock& clock) : clock_(clock), rng_(seed()) {
}

std::string MonotonicUlidGenerator::next_ulid() {
  const std::lock_guard lock(mutex_);
  const std::int64_t now_ms = unix_millis(clock_.system_now());
  if (now_ms <= last_ms_) {
    // 同じミリ秒（または時計の戻り）：前回の乱数部に1を足す
    for (std::size_t i = last_random_.size(); i-- > 0;) {
      if (++last_random_.at(i) != 0) {
        break;
      }
    }
  } else {
    last_ms_ = now_ms;
    for (auto& byte : last_random_) {
      byte = static_cast<std::uint8_t>(rng_() & kByteMask);
    }
  }
  return encode_ulid(static_cast<std::uint64_t>(last_ms_), last_random_);
}

SequentialIdGenerator::SequentialIdGenerator(const IClock& clock) : clock_(clock) {
}

std::string SequentialIdGenerator::next_ulid() {
  ++counter_;
  UlidRandom random{};
  for (std::size_t i = 0; i < random.size(); ++i) {
    const auto shift = 8 * (random.size() - 1 - i);
    random.at(i) = shift < 64 ? static_cast<std::uint8_t>((counter_ >> shift) & kByteMask) : 0;
  }
  return encode_ulid(static_cast<std::uint64_t>(unix_millis(clock_.system_now())), random);
}

std::uint32_t random_boot_id() {
  std::random_device device;
  return static_cast<std::uint32_t>(device());
}

std::string format_boot_id(std::uint32_t boot_id) {
  std::array<char, 9> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%08x", boot_id);
  return {buffer.data(), 8};
}

}  // namespace gw::core
