#include "core/mode/SystemModeController.h"

#include <utility>

namespace gw::core::mode {

std::string_view severity_of(std::string_view mode) {
  if (mode == kEmergency) {
    return "critical";
  }
  if (mode == kAlert) {
    return "warning";
  }
  return "info";
}

bool is_mode(std::string_view mode) {
  return mode == kNormal || mode == kAlert || mode == kEmergency;
}

bool SystemModeController::suppressed(command::Priority p) const {
  return emergency() && (p == command::Priority::kAuto || p == command::Priority::kSchedule);
}

ModeChange SystemModeController::change(std::string to, std::string reason) {
  ModeChange c{mode_, std::move(to), std::move(reason)};
  mode_ = c.to;
  return c;
}

std::optional<ModeChange> SystemModeController::raise(std::string_view to,
                                                      const std::string& rule_id) {
  if (to == kEmergency) {
    emergency_rules_.insert(rule_id);
    if (mode_ == kEmergency) {
      return std::nullopt;
    }
    return change(std::string{kEmergency}, rule_id);
  }
  if (to == kAlert) {
    alert_rules_.insert(rule_id);  // EMERGENCY 中でも覚えておく（承認のあとに ALERT へ）
    if (mode_ != kNormal) {
      return std::nullopt;
    }
    return change(std::string{kAlert}, rule_id);
  }
  return std::nullopt;
}

std::optional<ModeChange> SystemModeController::approve(std::string reason) {
  if (mode_ == kNormal) {
    return std::nullopt;
  }
  if (mode_ == kEmergency) {
    emergency_rules_.clear();
  } else {
    alert_rules_.clear();
  }
  return change(std::string{kNormal}, std::move(reason));
}

}  // namespace gw::core::mode
