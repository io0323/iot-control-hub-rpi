// ルールエンジン（基本設計 第6章「ルール定義」「時間条件と値の鮮度」、FR-07）。
//
// - 状態：idle → pending（成立、for の経過待ち）→ active（発火）→ cooldown → idle。
//   pending 中に不成立・不明なら idle
// - 評価は「値が更新されたチャネルを参照するルールだけ」（索引は構築時に作る）。加えて for・cooldown の
//   期限、1秒ごとの時刻帯と stale の変化、生存状態・モードの変化でも評価する
// - 条件は三値（真・偽・不明）。stale・quality が good でない・まだ値が無いチャネルは不明
//   （open-questions #30）。active のまま不明になったら kUnknown（新しい指令なし・リース更新停止）、
//   真に戻ったら kResumed
// - ヒステリシスは active の間だけ大小比較の葉に効く（open-questions #31）
// - 手動の release で所有を外されたルールは silence：次に発火するまで指令もリース更新もしない
//
// 指令・モード・通知などのアクションの実行は Core が効果（RuleEffect）を受けて行う。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/common/Clock.h"
#include "core/rules/Rule.h"

namespace gw::core::rules {

enum class Tri : std::uint8_t { kFalse, kTrue, kUnknown };

// 評価に使う値（Core が DeviceRegistry・SystemModeController・時計から答える）
class RuleContext {
 public:
  RuleContext() = default;
  RuleContext(const RuleContext&) = delete;
  RuleContext& operator=(const RuleContext&) = delete;
  RuleContext(RuleContext&&) = delete;
  RuleContext& operator=(RuleContext&&) = delete;
  virtual ~RuleContext() = default;
  // 判定に使える最新値。未登録・まだ値が無い・stale・quality が good でないなら nullopt（不明）
  [[nodiscard]] virtual std::optional<double> value(const std::string& device,
                                                    const std::string& channel) const = 0;
  // device の状態が status か（"*" はどれか1つ）。まだ観測していないデバイスは偽
  [[nodiscard]] virtual bool status_is(const std::string& device,
                                       device::DeviceStatus status) const = 0;
  [[nodiscard]] virtual std::string mode() const = 0;
  // 現地時刻（gateway.yaml の timezone）の 0:00 からの分
  [[nodiscard]] virtual int local_minutes() const = 0;
};

enum class RuleState : std::uint8_t { kIdle, kPending, kActive, kCooldown };
[[nodiscard]] std::string_view to_string(RuleState s);

struct RuleEffect {
  enum class Kind : std::uint8_t {
    kFired,    // 発火：アクションを実行し RULE_FIRED
    kCleared,  // 解除：RULE_CLEARED、hold の release
    kUnknown,  // active のまま不明：新しい指令を出さずリース更新を止める
    kResumed,  // 不明から真に戻った：hold の指令を送り直して更新を再開する
  };
  Kind kind = Kind::kFired;
  std::size_t rule = 0;  // RuleEngine::rules() の添字
};

using ChannelKey = std::pair<std::string, std::string>;  // (device, channel)

class RuleEngine {
 public:
  RuleEngine() = default;
  explicit RuleEngine(RuleSet set);

  [[nodiscard]] const std::vector<Rule>& rules() const { return rules_; }
  [[nodiscard]] std::optional<std::size_t> find(const std::string& id) const;
  [[nodiscard]] RuleState state(std::size_t rule) const { return runtime_.at(rule).state; }
  [[nodiscard]] bool unknown(std::size_t rule) const { return runtime_.at(rule).unknown; }
  [[nodiscard]] bool silenced(std::size_t rule) const { return runtime_.at(rule).silenced; }
  [[nodiscard]] std::uint64_t evaluations() const { return evaluations_; }

  // 値が更新された：そのチャネルを参照するルールだけを評価する
  void on_channel(const ChannelKey& key, const RuleContext& ctx, SteadyTime now,
                  std::vector<RuleEffect>& out);
  // 生存状態が変わった
  void on_status(const std::string& device, const RuleContext& ctx, SteadyTime now,
                 std::vector<RuleEffect>& out);
  // システムモードが変わった
  void on_mode(const RuleContext& ctx, SteadyTime now, std::vector<RuleEffect>& out);
  // 1秒ごと：時刻帯を参照するルールと、stale の状態が変わったチャネルを参照するルール
  void on_tick(const std::set<ChannelKey>& stale_changed, const RuleContext& ctx, SteadyTime now,
               std::vector<RuleEffect>& out);
  // for・cooldown の期限
  void on_timer(const RuleContext& ctx, SteadyTime now, std::vector<RuleEffect>& out);
  // 全ルールを評価する（起動時・再読込の後）
  void evaluate_all(const RuleContext& ctx, SteadyTime now, std::vector<RuleEffect>& out);
  // 全ルールを解除する（再読込の前）。active だったものは kCleared
  void clear_all(std::vector<RuleEffect>& out);
  // 手動の release で所有を外された：次に発火するまで指令もリース更新もしない（#26）
  void silence(std::size_t rule) { runtime_.at(rule).silenced = true; }

  [[nodiscard]] std::optional<SteadyTime> next_deadline() const;

  // 条件の評価（テストからも使う）。active なら hysteresis を効かせる
  [[nodiscard]] static Tri eval(const Condition& c, const Rule& rule, bool active,
                                const RuleContext& ctx);

 private:
  struct Runtime {
    RuleState state = RuleState::kIdle;
    std::optional<SteadyTime> deadline;  // pending の for、cooldown の終わり
    bool unknown = false;
    bool silenced = false;
  };

  void evaluate(std::size_t i, const RuleContext& ctx, SteadyTime now,
                std::vector<RuleEffect>& out);
  void fire(std::size_t i, std::vector<RuleEffect>& out);
  void index(std::size_t i, const Condition& c);

  std::vector<Rule> rules_;
  std::vector<Runtime> runtime_;
  std::map<ChannelKey, std::set<std::size_t>> by_channel_;
  std::map<std::string, std::set<std::size_t>> by_status_;  // device（"*" を含む）
  std::set<std::size_t> by_mode_;
  std::set<std::size_t> by_time_;
  std::uint64_t evaluations_ = 0;
};

}  // namespace gw::core::rules
