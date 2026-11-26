#include "core/common/TimerService.h"

namespace gw::core {

void TimerService::schedule(SteadyTime at, TimerKey key) {
  const auto generation = next_generation_++;
  live_[key] = generation;
  heap_.push(Entry{at, next_order_++, key, generation});
}

void TimerService::cancel(TimerKey key) {
  live_.erase(key);
}

bool TimerService::scheduled(TimerKey key) const {
  return live_.contains(key);
}

void TimerService::drop_stale() {
  while (!heap_.empty()) {
    const auto& top = heap_.top();
    const auto it = live_.find(top.key);
    if (it != live_.end() && it->second == top.generation) {
      return;
    }
    heap_.pop();
  }
}

std::optional<SteadyTime> TimerService::next_deadline() {
  drop_stale();
  if (heap_.empty()) {
    return std::nullopt;
  }
  return heap_.top().at;
}

std::vector<TimerKey> TimerService::pop_due(SteadyTime now) {
  std::vector<TimerKey> due;
  for (;;) {
    drop_stale();
    if (heap_.empty() || heap_.top().at > now) {
      return due;
    }
    const auto key = heap_.top().key;
    heap_.pop();
    live_.erase(key);
    due.push_back(key);
  }
}

}  // namespace gw::core
