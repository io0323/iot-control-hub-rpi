#include "core/quality/Quality.h"

#include <stdexcept>

namespace gw::core::quality {

bool is_clock_skew(SystemTime ts, SystemTime received_at) {
  const auto diff = received_at > ts ? received_at - ts : ts - received_at;
  return diff > kClockSkewThreshold;
}

bool is_late(SystemTime ts, SystemTime received_at, std::int64_t period_ms) {
  const auto age = received_at - ts;
  const auto threshold = std::chrono::milliseconds{period_ms * kLatePeriodMultiplier};
  return threshold < age && age <= kLateMaxAge;
}

bool exempt_from_seq_checks(const Json& message) {
  if (!message.is_object() || !message.contains("schema") || message["schema"] != "iot.v1.state") {
    return false;
  }
  const auto& payload = message.contains("payload") ? message["payload"] : Json();
  return payload.is_object() && payload.contains("status") && payload["status"] == "offline";
}

DedupWindow::DedupWindow(std::size_t capacity) : capacity_(capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("capacity は1以上");
  }
}

bool DedupWindow::is_duplicate(const std::string& device_id, const std::string& boot_id,
                               const std::string& kind, std::uint32_t seq) {
  auto& window = windows_[{device_id, kind}];
  if (window.boot_id != boot_id) {
    window = Window{boot_id, {}, {}};
  }
  if (window.seen.contains(seq)) {
    return true;
  }
  window.order.push_back(seq);
  window.seen.insert(seq);
  if (window.order.size() > capacity_) {
    window.seen.erase(window.order.front());
    window.order.pop_front();
  }
  return false;
}

std::size_t DedupWindow::remembered(const std::string& device_id, const std::string& kind) const {
  const auto it = windows_.find({device_id, kind});
  return it == windows_.end() ? 0 : it->second.order.size();
}

std::uint64_t GapCounter::observe(const std::string& device_id, const std::string& boot_id,
                                  const std::string& kind, std::uint32_t seq) {
  const auto key = std::make_pair(device_id, kind);
  const auto it = last_.find(key);
  if (it == last_.end() || it->second.boot_id != boot_id) {
    last_[key] = Last{boot_id, seq};
    return 0;
  }
  auto& last = it->second;
  if (seq <= last.seq) {
    return 0;  // 遅れて届いた・再配送（重複は呼び出す前に除いてある）
  }
  const std::uint64_t gap = static_cast<std::uint64_t>(seq) - last.seq - 1;
  last.seq = seq;
  return gap;
}

bool ClockSkewLimiter::allow(const std::string& device_id, SteadyTime now) {
  const auto it = last_.find(device_id);
  if (it != last_.end() && now - it->second < kClockSkewEventInterval) {
    return false;
  }
  last_[device_id] = now;
  return true;
}

}  // namespace gw::core::quality
