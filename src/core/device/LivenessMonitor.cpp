#include "core/device/LivenessMonitor.h"

#include <algorithm>

#include "core/quality/Quality.h"

namespace gw::core::device {

namespace {

bool available(std::optional<DeviceStatus> status) {
  return status == DeviceStatus::kOnline || status == DeviceStatus::kSuspect;
}

}  // namespace

std::optional<LivenessChange> LivenessMonitor::set_status(const std::string& device_id,
                                                          DeviceStatus to) {
  auto* info = registry_.find(device_id);
  if (info == nullptr || info->status == to) {
    return std::nullopt;
  }
  LivenessChange change{device_id, info->status, to, {}};
  if (to == DeviceStatus::kOffline && available(info->status)) {
    change.event_code = "DEVICE_OFFLINE";
  } else if (to == DeviceStatus::kOnline && info->status == DeviceStatus::kOffline) {
    change.event_code = "DEVICE_ONLINE";
  }
  info->status = to;
  return change;
}

void LivenessMonitor::uart_frame(const std::string& device_id, SteadyTime now) {
  uart_last_frame_[device_id] = now;
  auto* info = registry_.find(device_id);
  if (info != nullptr && info->status == DeviceStatus::kSuspect) {
    (void)set_status(device_id, DeviceStatus::kOnline);  // suspect の間にフレームが戻った
  }
}

std::vector<LivenessChange> LivenessMonitor::uart_check(SteadyTime now) {
  std::vector<LivenessChange> changes;
  for (const auto& [device_id, last] : uart_last_frame_) {
    const auto* info = registry_.find(device_id);
    if (info == nullptr || !available(info->status)) {
      continue;  // offline（次の STATE まで）と未観測は見ない
    }
    const auto elapsed = now - last;
    std::optional<LivenessChange> change;
    if (elapsed >= kUartOfflineAfter) {
      change = set_status(device_id, DeviceStatus::kOffline);
    } else if (elapsed >= kUartSuspectAfter) {
      change = set_status(device_id, DeviceStatus::kSuspect);
    }
    if (change) {
      changes.push_back(std::move(*change));
    }
  }
  return changes;
}

std::optional<SteadyTime> LivenessMonitor::uart_next_deadline() const {
  std::optional<SteadyTime> next;
  for (const auto& [device_id, last] : uart_last_frame_) {
    const auto* info = registry_.find(device_id);
    if (info == nullptr || !available(info->status)) {
      continue;
    }
    const auto at =
        info->status == DeviceStatus::kOnline ? last + kUartSuspectAfter : last + kUartOfflineAfter;
    next = next ? std::min(*next, at) : at;
  }
  return next;
}

std::vector<LivenessChange> LivenessMonitor::stale_check(SteadyTime now) {
  std::vector<LivenessChange> changes;
  for (auto& [device_id, info] : registry_.devices()) {
    bool all_stale = !info.channels.empty();
    for (auto& [name, ch] : info.channels) {
      ch.stale = false;
      if (const auto& last = ch.last_update; last && ch.period_ms > 0) {
        ch.stale = quality::is_stale(now - *last, ch.period_ms);
      }
      // 一度も値を受けていないチャネルは、state を受けてから数えない（suspect の根拠にしない）
      all_stale = all_stale && ch.stale;
    }
    if (info.transport == "mqtt" && all_stale && info.status == DeviceStatus::kOnline) {
      if (auto change = set_status(device_id, DeviceStatus::kSuspect)) {
        changes.push_back(std::move(*change));
      }
    }
  }
  return changes;
}

std::optional<LivenessChange> LivenessMonitor::fresh_value(const std::string& device_id) {
  const auto* info = registry_.find(device_id);
  if (info == nullptr || info->status != DeviceStatus::kSuspect) {
    return std::nullopt;
  }
  return set_status(device_id, DeviceStatus::kOnline);
}

}  // namespace gw::core::device
