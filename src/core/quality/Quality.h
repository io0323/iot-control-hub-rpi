// 受信側が判定するデータ品質（共通仕様 第5章）。
// 対応する Python：iotspec.quality（is_stale・classify_clock_skew・classify_late）と
// iotspec.dedup（SeqDedupWindow・exempt_from_seq_checks）。
// 境界の扱いは spec.md の語に合わせる：「超えた」「超」は厳密に大きい場合、「以内」は等しい場合を含む。
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "core/common/Clock.h"
#include "core/common/Constants.h"
#include "core/common/Envelope.h"

namespace gw::core::quality {

inline constexpr int kStalePeriodMultiplier = constants::kStalePeriodMultiplier;
inline constexpr int kLatePeriodMultiplier = constants::kLatePeriodMultiplier;
inline constexpr std::chrono::seconds kClockSkewThreshold{constants::kClockSkewThresholdS};
inline constexpr std::chrono::seconds kLateMaxAge{constants::kLateMaxAgeS};
inline constexpr std::chrono::seconds kClockSkewEventInterval{constants::kClockSkewEventIntervalS};

// 最終更新から period_ms の3倍を超えたら true（ちょうど3倍は false）。is_stale
template <typename Duration>
[[nodiscard]] bool is_stale(Duration elapsed, std::int64_t period_ms) {
  return elapsed > std::chrono::milliseconds{period_ms * kStalePeriodMultiplier};
}

// ts と受信時刻の差が±5分を超えたら true（ちょうど5分は false）。classify_clock_skew
[[nodiscard]] bool is_clock_skew(SystemTime ts, SystemTime received_at);

// ts が受信時刻より過去で、差が周期の3倍を超え、かつ5分以内なら true。classify_late
[[nodiscard]] bool is_late(SystemTime ts, SystemTime received_at, std::int64_t period_ms);

// seq による重複・欠損・遅延の判定から外す電文（status: offline の state。v0.2.1 #29）。
// exempt_from_seq_checks
[[nodiscard]] bool exempt_from_seq_checks(const Json& message);

// 発信元・kind ごとに直近 capacity 個の seq を記憶する重複判定器（SeqDedupWindow）。
// - 記憶にある seq が再び来たら重複
// - boot_id が変わったら、その発信元・kind の記憶を捨てて新しい起動として数える
// - 記憶は受け取った順に古いものから捨てる。捨てた seq が再び来ても重複とは判定しない
class DedupWindow {
 public:
  explicit DedupWindow(std::size_t capacity);
  // 重複なら true。初めての seq なら記憶して false
  [[nodiscard]] bool is_duplicate(const std::string& device_id, const std::string& boot_id,
                                  const std::string& kind, std::uint32_t seq);
  [[nodiscard]] std::size_t remembered(const std::string& device_id, const std::string& kind) const;

 private:
  struct Window {
    std::string boot_id;
    std::deque<std::uint32_t> order;
    std::unordered_set<std::uint32_t> seen;
  };
  std::size_t capacity_;
  std::map<std::pair<std::string, std::string>, Window> windows_;
};

// 欠損：同じ boot_id 内での seq の飛び（第5章）。発信元・kind ごとに最後の seq を覚え、飛んだ数を返す。
// boot_id が変わったら数え直す。戻った seq（遅れて届いた・再配送）は欠損に数えない
class GapCounter {
 public:
  // 今回の seq までに飛んだ数（飛んでいなければ0）
  std::uint64_t observe(const std::string& device_id, const std::string& boot_id,
                        const std::string& kind, std::uint32_t seq);

 private:
  struct Last {
    std::string boot_id;
    std::uint32_t seq = 0;
  };
  std::map<std::pair<std::string, std::string>, Last> last_;
};

// CLOCK_SKEW は同一デバイス1時間に1回まで（第5章）
class ClockSkewLimiter {
 public:
  // 今回発行してよければ true（記録する）
  bool allow(const std::string& device_id, SteadyTime now);

 private:
  std::map<std::string, SteadyTime> last_;
};

}  // namespace gw::core::quality
