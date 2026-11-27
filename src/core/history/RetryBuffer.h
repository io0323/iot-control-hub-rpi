// 上限つきの履歴の保持（基本設計 第8章「DB書き込み失敗」：メモリに最大1万件を保持して再試行）。
// あふれたら、区分（HistoryRecord.h）の小さいものから、同じ区分では古いものから捨てる（#41）。
// 上限は必ず守る。history キュー（8192件）のあふれも同じ規則（evict_index）で捨てる。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "core/history/HistoryRecord.h"

namespace gw::core::history {

// 並び（古い順）と新しい1件から、捨てるものを選ぶ。並びの添字、新しい1件なら nullopt
template <typename Container>
[[nodiscard]] std::optional<std::size_t> evict_index(const Container& items, Tier incoming) {
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto t = items[i].tier;
    if (!best || t < items[*best].tier) {
      best = i;  // 同じ区分では最初（いちばん古い）のまま
    }
  }
  if (!best || incoming < items[*best].tier) {
    return std::nullopt;  // 新しい1件のほうが区分が小さい：それを捨てる
  }
  return best;
}

class RetryBuffer {
 public:
  static constexpr std::size_t kDefaultCapacity = 10'000;

  explicit RetryBuffer(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  // 1件足す。あふれたら1件捨てて true（捨てた数は dropped に数える）
  bool push(HistoryRecord r);
  void push_all(std::vector<HistoryRecord> records);
  // 先頭から最大 n 件を取り出す（書き込みに使う）。失敗したら restore で戻す
  [[nodiscard]] std::vector<HistoryRecord> take(std::size_t n);
  // 書き込めなかった分を先頭に戻す（あふれたら規則どおり捨てる）
  void restore(std::vector<HistoryRecord> records);

  [[nodiscard]] std::size_t size() const { return items_.size(); }
  [[nodiscard]] bool empty() const { return items_.empty(); }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  // 捨てた件数（区分ごと）。DATA_DROPPED で知らせたら reset する
  [[nodiscard]] std::uint64_t dropped() const;
  [[nodiscard]] std::uint64_t dropped(Tier t) const { return dropped_.at(static_cast<int>(t)); }
  void reset_dropped() { dropped_ = {}; }

 private:
  std::size_t capacity_;
  std::deque<HistoryRecord> items_;
  std::array<std::uint64_t, kTierCount> dropped_{};
};

}  // namespace gw::core::history
