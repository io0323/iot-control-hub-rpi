#include <algorithm>
#include <iterator>
#include <numeric>

#include "core/history/HistoryRecord.h"
#include "core/history/RetryBuffer.h"

namespace gw::core::history {

Tier event_tier(std::string_view severity) {
  if (severity == "critical") {
    return Tier::kCriticalEvent;
  }
  if (severity == "warning") {
    return Tier::kWarningEvent;
  }
  return Tier::kInfoEvent;
}

bool is_final_state(std::string_view state) {
  return state == "DONE" || state == "REJECTED" || state == "FAILED" || state == "TIMED_OUT" ||
         state == "done" || state == "rejected" || state == "failed";
}

HistoryRecord record(Row row) {
  const Tier tier = std::visit(
      [](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, EventRow>) {
          return event_tier(r.severity);
        } else if constexpr (std::is_same_v<T, DeviceRow> || std::is_same_v<T, DeviceStatusRow>) {
          return Tier::kDeviceStatus;
        } else if constexpr (std::is_same_v<T, RuleFiringRow>) {
          return Tier::kRuleFiring;
        } else if constexpr (std::is_same_v<T, CommandRow>) {
          return is_final_state(r.state) ? Tier::kFinal : Tier::kCommandTransition;
        } else if constexpr (std::is_same_v<T, CommandTransitionRow>) {
          return is_final_state(r.to_state) ? Tier::kFinal : Tier::kCommandTransition;
        } else if constexpr (std::is_same_v<T, RequestRow>) {
          return is_final_state(r.result) ? Tier::kFinal : Tier::kRequestProgress;
        } else if constexpr (std::is_same_v<T, ModeRow>) {
          return Tier::kModeChange;
        } else {
          static_assert(std::is_same_v<T, DiagRow>);
          return Tier::kDiag;
        }
      },
      row);
  return HistoryRecord{std::move(row), tier};
}

bool RetryBuffer::push(HistoryRecord r) {
  if (items_.size() < capacity_) {
    items_.push_back(std::move(r));
    return false;
  }
  const auto victim = evict_index(items_, r.tier);
  if (!victim) {
    ++dropped_.at(static_cast<int>(r.tier));  // 新しい1件を捨てる
    return true;
  }
  ++dropped_.at(static_cast<int>(items_[*victim].tier));
  items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(*victim));
  items_.push_back(std::move(r));
  return true;
}

void RetryBuffer::push_all(std::vector<HistoryRecord> records) {
  for (auto& r : records) {
    (void)push(std::move(r));
  }
}

std::vector<HistoryRecord> RetryBuffer::take(std::size_t n) {
  const auto count = std::min(n, items_.size());
  std::vector<HistoryRecord> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(std::move(items_.front()));
    items_.pop_front();
  }
  return out;
}

void RetryBuffer::restore(std::vector<HistoryRecord> records) {
  // 先頭に戻して順序を保つ。そのあと上限まで規則どおりに捨てる
  items_.insert(items_.begin(), std::make_move_iterator(records.begin()),
                std::make_move_iterator(records.end()));
  while (items_.size() > capacity_) {
    const auto victim = evict_index(items_, Tier::kFinal);
    const auto i = victim.value_or(items_.size() - 1);
    ++dropped_.at(static_cast<int>(items_[i].tier));
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

std::uint64_t RetryBuffer::dropped() const {
  return std::accumulate(dropped_.begin(), dropped_.end(), std::uint64_t{0});
}

}  // namespace gw::core::history
