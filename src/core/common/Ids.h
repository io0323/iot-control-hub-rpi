// ID（詳細設計書 第4章「時刻と ID」）。
// - ULID：時刻部 48 ビット（Unix ミリ秒）＋乱数部 80 ビットを Crockford Base32 の26文字にする
// - MonotonicUlidGenerator：本番。同じミリ秒の中では乱数部を1ずつ増やす（単調 ULID）
// - SequentialIdGenerator：テスト用。gen_golden.py の _Session._new_id と同じ規則
//   （時刻部＝時計の現在時刻、乱数部＝1から始まる通し番号）
// - boot_id：起動ごとの32ビット乱数を16進小文字8桁
//   （iotspec.ids.new_boot_id、bridge.format_boot_id）
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>

#include "core/common/Clock.h"

namespace gw::core {

using UlidRandom = std::array<std::uint8_t, 10>;

[[nodiscard]] std::string encode_ulid(std::uint64_t unix_ms, const UlidRandom& random);

class IIdGenerator {
 public:
  IIdGenerator() = default;
  IIdGenerator(const IIdGenerator&) = delete;
  IIdGenerator& operator=(const IIdGenerator&) = delete;
  IIdGenerator(IIdGenerator&&) = delete;
  IIdGenerator& operator=(IIdGenerator&&) = delete;
  virtual ~IIdGenerator() = default;

  // msg_id・cmd_id に使う ULID
  [[nodiscard]] virtual std::string next_ulid() = 0;
};

class MonotonicUlidGenerator final : public IIdGenerator {
 public:
  explicit MonotonicUlidGenerator(const IClock& clock);
  [[nodiscard]] std::string next_ulid() override;

 private:
  const IClock& clock_;
  std::mutex mutex_;
  std::mt19937_64 rng_;
  std::int64_t last_ms_ = -1;
  UlidRandom last_random_{};
};

class SequentialIdGenerator final : public IIdGenerator {
 public:
  explicit SequentialIdGenerator(const IClock& clock);
  [[nodiscard]] std::string next_ulid() override;

 private:
  const IClock& clock_;
  std::uint64_t counter_ = 0;
};

[[nodiscard]] std::uint32_t random_boot_id();
// boot_id:u32 → 16進小文字8桁
[[nodiscard]] std::string format_boot_id(std::uint32_t boot_id);

}  // namespace gw::core
