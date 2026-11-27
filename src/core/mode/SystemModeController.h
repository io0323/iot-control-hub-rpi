// システムモード（基本設計 第6章「システムモード」、FR-12。共通仕様 v0.2.3 #46・#48）。
// NORMAL・ALERT・EMERGENCY の遷移と、その影響の判定だけを持つ。入る条件はルールで書く。
//
// - ルールの set_system_mode：ALERT は NORMAL からだけ上げる。EMERGENCY はどこからでも入る
// - ALERT → NORMAL：ALERT に入れたルールがすべて active でなくなったら自動で戻る（open-questions #29）
// - EMERGENCY → NORMAL：条件の回復（EMERGENCY に入れたルールがすべて idle）＋承認（#46）
// - 影響：EMERGENCY では schedule の request を拒否し（emergency_mode）、auto・schedule のルールの
//   指令を止める（#34）。ALERT は通知の強化だけ（#28）
#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "core/command/Priority.h"

namespace gw::core::mode {

inline constexpr std::string_view kNormal = "NORMAL";
inline constexpr std::string_view kAlert = "ALERT";
inline constexpr std::string_view kEmergency = "EMERGENCY";
inline constexpr std::string_view kApprovedByRequest = "approved_by_request";  // #48
inline constexpr std::string_view kApprovedByGwctl = "approved_by_gwctl";

// SYSTEM_MODE_CHANGED の内容（#48：重大度は遷移先で決める）
struct ModeChange {
  std::string from;
  std::string to;
  std::string reason;
};
[[nodiscard]] std::string_view severity_of(std::string_view mode);
[[nodiscard]] bool is_mode(std::string_view mode);

class SystemModeController {
 public:
  [[nodiscard]] const std::string& mode() const { return mode_; }
  [[nodiscard]] bool emergency() const { return mode_ == kEmergency; }
  // EMERGENCY で止める優先度か（#34：auto・schedule）
  [[nodiscard]] bool suppressed(command::Priority p) const;

  // ルールの set_system_mode（ALERT・EMERGENCY）。モードが変わったら ModeChange
  [[nodiscard]] std::optional<ModeChange> raise(std::string_view to, const std::string& rule_id);
  // ルールが active でなくなった：ALERT に入れたルールがすべて active でなければ NORMAL に戻す。
  // still_active(rule_id) は Core が RuleEngine から答える
  template <typename StillActive>
  [[nodiscard]] std::optional<ModeChange> rule_cleared(const std::string& rule_id,
                                                       const StillActive& still_active) {
    if (mode_ != kAlert || !alert_rules_.contains(rule_id)) {
      return std::nullopt;
    }
    for (const auto& id : alert_rules_) {
      if (still_active(id)) {
        return std::nullopt;
      }
    }
    alert_rules_.clear();
    return change(std::string{kNormal}, rule_id);
  }
  // 承認（#46）：NORMAL に戻す。recovered は Core が判定する（今のモードに入れたルールの状態）
  [[nodiscard]] std::optional<ModeChange> approve(std::string reason);

  // 今のモードに入れたルール（EMERGENCY なら EMERGENCY に、ALERT なら ALERT に入れたもの）
  [[nodiscard]] const std::set<std::string>& entered_by() const {
    return emergency() ? emergency_rules_ : alert_rules_;
  }
  [[nodiscard]] const std::set<std::string>& alert_rules() const { return alert_rules_; }
  // 再起動をまたぐ EMERGENCY（Phase 5）：EMERGENCY で起動し、承認を待つ。rule は入れたルール（open-questions #44）
  void restore_emergency(const std::optional<std::string>& rule) {
    mode_ = std::string{kEmergency};
    if (rule) {
      emergency_rules_.insert(*rule);
    }
  }
  // 再読込でルールが消えた：記録からも外す
  void forget_rules() {
    alert_rules_.clear();
    emergency_rules_.clear();
  }

 private:
  [[nodiscard]] ModeChange change(std::string to, std::string reason);

  std::string mode_{kNormal};
  std::set<std::string> alert_rules_;
  std::set<std::string> emergency_rules_;
};

}  // namespace gw::core::mode
