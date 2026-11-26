// タイマー（詳細設計書 第4章「TimerService」）。最小ヒープ。
// 取り消しは世代番号で無効化する（ヒープからは消さない）。同じキーの schedule
// は前の予定を置き換える。
#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <queue>
#include <vector>

#include "core/common/Clock.h"

namespace gw::core {

enum class TimerKind : std::uint8_t {
  kUartHeartbeat,  // UART HEARTBEAT 送信（1秒）
  kUartLiveness,   // UART 生存確認（最後のフレーム受信＋3秒）
  kUartCmdSlot,    // UART の指令の毎秒1件
  kStaleCheck,     // stale の判定（1秒）
};

struct TimerKey {
  TimerKind kind{};
  std::uint64_t id = 0;
  // clang-tidy 15 は既定の <=> に modernize-use-nullptr を誤って出す
  auto operator<=>(const TimerKey&) const = default;  // NOLINT(modernize-use-nullptr)
};

class TimerService {
 public:
  void schedule(SteadyTime at, TimerKey key);
  void cancel(TimerKey key);
  [[nodiscard]] bool scheduled(TimerKey key) const;
  [[nodiscard]] std::optional<SteadyTime> next_deadline();
  // now までに期限が来たキーを、期限の早い順に返す（取り除く）
  [[nodiscard]] std::vector<TimerKey> pop_due(SteadyTime now);

 private:
  struct Entry {
    SteadyTime at;
    std::uint64_t order;  // 同じ時刻は予定した順
    TimerKey key;
    std::uint64_t generation;
    bool operator>(const Entry& other) const {
      return at != other.at ? at > other.at : order > other.order;
    }
  };

  void drop_stale();

  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap_;
  std::map<TimerKey, std::uint64_t> live_;  // キー → 有効な世代
  std::uint64_t next_generation_ = 1;
  std::uint64_t next_order_ = 0;
};

}  // namespace gw::core
