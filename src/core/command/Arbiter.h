// 調停（基本設計 第6章「調停」、FR-11）。アクチュエーターごとの所有者と待ちを持つ。
//
// - 新しい主張は、優先度が現所有者以上なら受け入れて所有者を入れ替える。低ければ拒否
//   （lower_priority）。同じ優先度では新しい方が勝つ
// - 奪われた所有者は、自分のリースの期限まで待ちに残る。拒否された主張は待ちに入らない
//   （open-questions #24）
// - 所有者が解放・期限切れになったら、待ちの中で優先度が最も高く新しいものが引き継ぐ。
//   待ちが空なら所有者なし
// - マイコン側のインターロックはさらに上位で、ack の limited で知る（ここでは扱わない）
#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/command/Priority.h"
#include "core/common/Clock.h"
#include "core/common/Envelope.h"

namespace gw::core::command {

struct ActuatorKey {
  std::string device_id;
  std::string actuator;
  // clang-tidy 15 は既定の <=> に modernize-use-nullptr を誤って出す
  auto operator<=>(const ActuatorKey&) const = default;  // NOLINT(modernize-use-nullptr)
};

struct Claim {
  std::string owner;  // 主張の主（例：request:<request_id>、rule:<name>）
  Priority priority = Priority::kAuto;
  Json args;    // set_actuator の args（引き継ぎで送り直す値）
  Json origin;  // cmd の origin
  SteadyTime lease_until{};
  std::uint64_t order = 0;  // Arbiter が付ける。新しいほど大きい
};

struct ClaimResult {
  bool accepted = false;
  std::optional<Priority> owner_priority;  // 拒否したときの現所有者の優先度
};

// 所有者が変わった（解放・期限切れ）。new_owner が空なら所有者なし
struct OwnerChange {
  ActuatorKey key;
  std::optional<Claim> new_owner;
};

class Arbiter {
 public:
  [[nodiscard]] ClaimResult claim(const ActuatorKey& key, Claim c, SteadyTime now);
  // owner の主張（所有・待ち）を外す。所有者が外れたときだけ OwnerChange を返す
  [[nodiscard]] std::optional<OwnerChange> release(const ActuatorKey& key, const std::string& owner,
                                                   SteadyTime now);
  // priority からの解放（request の release、open-questions #26）：priority の待ちを外し、
  // 所有者の優先度が priority 以下なら所有者も外す。所有者が外れたときだけ OwnerChange を返す
  [[nodiscard]] std::optional<OwnerChange> release_for(const ActuatorKey& key, Priority priority,
                                                       SteadyTime now);
  // リースの期限（lease_until）に達した所有・待ちを外す
  [[nodiscard]] std::vector<OwnerChange> expire(SteadyTime now);
  // リースの更新で期限を延ばす
  void extend(const ActuatorKey& key, const std::string& owner, SteadyTime lease_until);

  [[nodiscard]] const Claim* owner(const ActuatorKey& key) const;
  [[nodiscard]] std::size_t waiting(const ActuatorKey& key) const;
  [[nodiscard]] std::optional<SteadyTime> next_expiry() const;

 private:
  struct Slot {
    std::optional<Claim> owner;
    std::vector<Claim> waiting;
  };
  // 待ちから次の所有者を選ぶ（期限内で優先度が最も高く新しいもの）
  static std::optional<Claim> take_next(Slot& slot, SteadyTime now);
  std::optional<OwnerChange> remove_if(const ActuatorKey& key, SteadyTime now,
                                       const auto& predicate);

  std::map<ActuatorKey, Slot> slots_;
  std::uint64_t next_order_ = 1;
};

}  // namespace gw::core::command
