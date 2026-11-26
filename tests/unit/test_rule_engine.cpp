// RuleEngine（基本設計 第6章「ルール定義」「時間条件と値の鮮度」）を仮想の時刻で確かめる：
// 9.9秒では発火しない・10秒で発火・途中で値が途絶えたら発火しない、ヒステリシスの両方向、cooldown、
// stale（不明）と再開、all・any・not の三値と時刻帯・モード・生存状態、索引、silence、clear_all
#include <gtest/gtest.h>

#include <algorithm>
#include <map>

#include "core/rules/RuleEngine.h"

namespace r = gw::core::rules;
using gw::core::SteadyTime;
using gw::core::device::DeviceStatus;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

const r::ChannelKey kTemp{"mcu-c-01", "temp_board"};
const r::ChannelKey kHum{"mcu-cpp-01", "humidity"};

class FakeContext final : public r::RuleContext {
 public:
  std::map<r::ChannelKey, double> values;  // 無いチャネルは不明
  std::map<std::string, DeviceStatus> statuses;
  std::string mode_value = "NORMAL";
  int minutes = 12 * 60;

  [[nodiscard]] std::optional<double> value(const std::string& device,
                                            const std::string& channel) const override {
    const auto it = values.find({device, channel});
    return it == values.end() ? std::nullopt : std::optional<double>{it->second};
  }
  [[nodiscard]] bool status_is(const std::string& device, DeviceStatus status) const override {
    if (device == r::kAnyDevice) {
      return std::ranges::any_of(statuses,
                                 [&](const auto& entry) { return entry.second == status; });
    }
    const auto it = statuses.find(device);
    return it != statuses.end() && it->second == status;
  }
  [[nodiscard]] std::string mode() const override { return mode_value; }
  [[nodiscard]] int local_minutes() const override { return minutes; }
};

r::Condition channel(const r::ChannelKey& key, r::Op op, double value) {
  r::Condition c;
  c.kind = r::Condition::Kind::kChannel;
  c.leaf = r::ChannelLeaf{key.first, key.second, op, value};
  return c;
}

r::Condition group(r::Condition::Kind kind, std::vector<r::Condition> children) {
  r::Condition c;
  c.kind = kind;
  c.children = std::move(children);
  return c;
}

r::Rule rule(std::string id, r::Condition when, milliseconds for_ = milliseconds{0},
             std::optional<double> hysteresis = std::nullopt,
             milliseconds cooldown = milliseconds{0}) {
  r::Rule rl;
  rl.id = std::move(id);
  rl.when = std::move(when);
  rl.for_duration = for_;
  rl.hysteresis = hysteresis;
  rl.cooldown = cooldown;
  rl.then.emplace_back(r::NotifyAction{});
  return rl;
}

std::vector<r::RuleEffect::Kind> kinds(const std::vector<r::RuleEffect>& effects) {
  std::vector<r::RuleEffect::Kind> result;
  for (const auto& e : effects) {
    result.push_back(e.kind);
  }
  return result;
}

using K = r::RuleEffect::Kind;

}  // namespace

TEST(RuleEngine, FiresAfterExactlyTenSecondsNotAtNinePointNine) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0), seconds{10})}});
  FakeContext ctx;
  const SteadyTime t0{};
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 46.0;
  engine.on_channel(kTemp, ctx, t0, out);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(engine.state(0), r::RuleState::kPending);
  EXPECT_EQ(engine.next_deadline(), t0 + seconds{10});
  engine.on_timer(ctx, t0 + milliseconds{9900}, out);
  EXPECT_TRUE(out.empty()) << "9.9秒で発火した";
  engine.on_timer(ctx, t0 + seconds{10}, out);
  EXPECT_EQ(kinds(out), std::vector<K>{K::kFired});
  EXPECT_EQ(engine.state(0), r::RuleState::kActive);
}

TEST(RuleEngine, DropoutDuringPendingCancels) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0), seconds{10})}});
  FakeContext ctx;
  const SteadyTime t0{};
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 46.0;
  engine.on_channel(kTemp, ctx, t0, out);
  ctx.values.erase(kTemp);  // stale になった（不明）
  engine.on_tick({kTemp}, ctx, t0 + seconds{5}, out);
  EXPECT_EQ(engine.state(0), r::RuleState::kIdle);
  ctx.values[kTemp] = 46.0;
  engine.on_timer(ctx, t0 + seconds{10}, out);
  EXPECT_TRUE(out.empty()) << "途絶えたのに発火した";
  // 値が戻ったら、そこから数え直す
  engine.on_channel(kTemp, ctx, t0 + seconds{11}, out);
  engine.on_timer(ctx, t0 + seconds{20}, out);
  EXPECT_TRUE(out.empty());
  engine.on_timer(ctx, t0 + seconds{21}, out);
  EXPECT_EQ(kinds(out), std::vector<K>{K::kFired});
}

TEST(RuleEngine, FalseDuringPendingCancelsAndTimerReevaluates) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0), seconds{10})}});
  FakeContext ctx;
  const SteadyTime t0{};
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 46.0;
  engine.on_channel(kTemp, ctx, t0, out);
  ctx.values[kTemp] = 44.0;
  engine.on_channel(kTemp, ctx, t0 + seconds{3}, out);
  EXPECT_EQ(engine.state(0), r::RuleState::kIdle);
  EXPECT_EQ(engine.next_deadline(), std::nullopt);
  // 値の更新を経ずに for の時刻に偽になっていた場合も、タイマーで評価し直して発火しない
  ctx.values[kTemp] = 46.0;
  engine.on_channel(kTemp, ctx, t0 + seconds{4}, out);
  ctx.values[kTemp] = 40.0;  // on_channel を呼ばずに値だけ変わった
  engine.on_timer(ctx, t0 + seconds{14}, out);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(engine.state(0), r::RuleState::kIdle);
}

TEST(RuleEngine, HysteresisUpward) {
  r::RuleEngine engine(
      r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0), milliseconds{0}, 2.0)}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  const auto step = [&](double v) {
    ctx.values[kTemp] = v;
    out.clear();
    engine.on_channel(kTemp, ctx, SteadyTime{}, out);
    return kinds(out);
  };
  EXPECT_TRUE(step(44.9).empty());
  EXPECT_EQ(step(45.0), std::vector<K>{K::kFired});
  EXPECT_TRUE(step(44.0).empty());  // 45 を下回っても 43 までは解除しない
  EXPECT_TRUE(step(43.0).empty());
  EXPECT_EQ(step(42.9), std::vector<K>{K::kCleared});
  EXPECT_TRUE(step(44.0).empty());  // 解除後は元のしきい値で判定する
  EXPECT_EQ(step(45.5), std::vector<K>{K::kFired});
}

TEST(RuleEngine, HysteresisDownward) {
  r::RuleEngine engine(
      r::RuleSet{{rule("dry", channel(kHum, r::Op::kLt, 30.0), milliseconds{0}, 5.0)}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  const auto step = [&](double v) {
    ctx.values[kHum] = v;
    out.clear();
    engine.on_channel(kHum, ctx, SteadyTime{}, out);
    return kinds(out);
  };
  EXPECT_TRUE(step(30.0).empty());
  EXPECT_EQ(step(29.0), std::vector<K>{K::kFired});
  EXPECT_TRUE(step(34.0).empty());
  EXPECT_TRUE(step(35.0).empty());
  EXPECT_EQ(step(35.1), std::vector<K>{K::kCleared});
}

TEST(RuleEngine, CooldownBlocksRefireUntilItEnds) {
  r::RuleEngine engine(r::RuleSet{
      {rule("hot", channel(kTemp, r::Op::kGe, 45.0), milliseconds{0}, std::nullopt, seconds{30})}});
  FakeContext ctx;
  const SteadyTime t0{};
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, t0, out);
  ctx.values[kTemp] = 40.0;
  engine.on_channel(kTemp, ctx, t0 + seconds{1}, out);
  EXPECT_EQ(kinds(out), (std::vector<K>{K::kFired, K::kCleared}));
  EXPECT_EQ(engine.state(0), r::RuleState::kCooldown);
  out.clear();
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, t0 + seconds{10}, out);
  EXPECT_TRUE(out.empty()) << "cooldown 中に発火した";
  engine.on_timer(ctx, t0 + milliseconds{30'999}, out);
  EXPECT_TRUE(out.empty());
  engine.on_timer(ctx, t0 + seconds{31}, out);  // cooldown が終わり、その時点の値で評価する
  EXPECT_EQ(kinds(out), std::vector<K>{K::kFired});
}

TEST(RuleEngine, StaleWhileActiveIsUnknownThenResumes) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0))}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  ctx.values.erase(kTemp);
  engine.on_tick({kTemp}, ctx, SteadyTime{}, out);
  engine.on_tick({kTemp}, ctx, SteadyTime{}, out);  // 2回目は何も出さない
  EXPECT_EQ(kinds(out), (std::vector<K>{K::kFired, K::kUnknown}));
  EXPECT_TRUE(engine.unknown(0));
  EXPECT_EQ(engine.state(0), r::RuleState::kActive);  // 解除はしない
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  EXPECT_EQ(out.back().kind, K::kResumed);
  EXPECT_FALSE(engine.unknown(0));
}

TEST(RuleEngine, ThreeValuedAllAnyNot) {
  FakeContext ctx;
  const auto hot = channel(kTemp, r::Op::kGe, 45.0);
  const auto humid = channel(kHum, r::Op::kGt, 70.0);
  const auto rl = rule("x", hot);
  const auto eval = [&](const r::Condition& c) { return r::RuleEngine::eval(c, rl, false, ctx); };
  using T = r::Tri;
  ctx.values[kTemp] = 50.0;  // 湿度は不明
  EXPECT_EQ(eval(group(r::Condition::Kind::kAny, {hot, humid})), T::kTrue);
  EXPECT_EQ(eval(group(r::Condition::Kind::kAll, {hot, humid})), T::kUnknown);
  EXPECT_EQ(eval(group(r::Condition::Kind::kNot, {humid})), T::kUnknown);
  EXPECT_EQ(eval(group(r::Condition::Kind::kNot, {hot})), T::kFalse);
  ctx.values[kTemp] = 40.0;
  EXPECT_EQ(eval(group(r::Condition::Kind::kAny, {hot, humid})), T::kUnknown);
  EXPECT_EQ(eval(group(r::Condition::Kind::kAll, {hot, humid})), T::kFalse);
  ctx.values[kHum] = 80.0;
  EXPECT_EQ(eval(group(r::Condition::Kind::kAny, {hot, humid})), T::kTrue);
  EXPECT_EQ(eval(group(r::Condition::Kind::kAll, {hot, group(r::Condition::Kind::kNot, {humid})})),
            T::kFalse);
  EXPECT_EQ(eval(group(r::Condition::Kind::kAll, {})), T::kTrue);
}

TEST(RuleEngine, OtherLeavesAndOperators) {
  FakeContext ctx;
  const auto rl = rule("x", r::Condition{});
  const auto eval = [&](const r::Condition& c) { return r::RuleEngine::eval(c, rl, false, ctx); };
  using T = r::Tri;
  r::Condition night;
  night.kind = r::Condition::Kind::kTime;
  night.leaf = r::TimeLeaf{22 * 60, 6 * 60};  // 日をまたぐ
  ctx.minutes = 23 * 60;
  EXPECT_EQ(eval(night), T::kTrue);
  ctx.minutes = 5 * 60 + 59;
  EXPECT_EQ(eval(night), T::kTrue);
  ctx.minutes = 6 * 60;
  EXPECT_EQ(eval(night), T::kFalse);
  r::Condition day;
  day.kind = r::Condition::Kind::kTime;
  day.leaf = r::TimeLeaf{9 * 60, 17 * 60};
  EXPECT_EQ(eval(day), T::kFalse);
  ctx.minutes = 9 * 60;
  EXPECT_EQ(eval(day), T::kTrue);
  day.leaf = r::TimeLeaf{0, 0};  // 終日
  EXPECT_EQ(eval(day), T::kTrue);

  r::Condition emergency;
  emergency.kind = r::Condition::Kind::kMode;
  emergency.leaf = r::ModeLeaf{"EMERGENCY"};
  EXPECT_EQ(eval(emergency), T::kFalse);
  ctx.mode_value = "EMERGENCY";
  EXPECT_EQ(eval(emergency), T::kTrue);

  r::Condition any_offline;
  any_offline.kind = r::Condition::Kind::kStatus;
  any_offline.leaf = r::StatusLeaf{"*", DeviceStatus::kOffline};
  r::Condition c01_offline = any_offline;
  c01_offline.leaf = r::StatusLeaf{"mcu-c-01", DeviceStatus::kOffline};
  EXPECT_EQ(eval(any_offline), T::kFalse);  // まだ観測していない
  ctx.statuses["mcu-cpp-01"] = DeviceStatus::kOffline;
  EXPECT_EQ(eval(any_offline), T::kTrue);
  EXPECT_EQ(eval(c01_offline), T::kFalse);

  ctx.values[kTemp] = 45.0;
  for (const auto& [op, expected] : std::vector<std::pair<r::Op, T>>{{r::Op::kGe, T::kTrue},
                                                                     {r::Op::kGt, T::kFalse},
                                                                     {r::Op::kLe, T::kTrue},
                                                                     {r::Op::kLt, T::kFalse},
                                                                     {r::Op::kEq, T::kTrue},
                                                                     {r::Op::kNe, T::kFalse}}) {
    EXPECT_EQ(eval(channel(kTemp, op, 45.0)), expected) << r::to_string(op);
    EXPECT_EQ(r::parse_op(r::to_string(op)), op);
  }
  EXPECT_EQ(r::parse_op("=>"), std::nullopt);
}

TEST(RuleEngine, OnlyRulesReferencingTheUpdatedChannelAreEvaluated) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0)),
                                   rule("humid", channel(kHum, r::Op::kGt, 70.0))}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  EXPECT_EQ(engine.evaluations(), 1U);
  engine.on_channel(r::ChannelKey{"mcu-cpp-01", "lux"}, ctx, SteadyTime{}, out);  // 関係ない
  EXPECT_EQ(engine.evaluations(), 1U);
  engine.on_tick({}, ctx, SteadyTime{}, out);  // 時刻帯のルールも stale の変化もない
  EXPECT_EQ(engine.evaluations(), 1U);
  engine.on_status("mcu-c-01", ctx, SteadyTime{}, out);  // 生存状態のルールはない
  engine.on_mode(ctx, SteadyTime{}, out);
  EXPECT_EQ(engine.evaluations(), 1U);
  engine.evaluate_all(ctx, SteadyTime{}, out);
  EXPECT_EQ(engine.evaluations(), 3U);
}

TEST(RuleEngine, StatusModeAndTimeRulesAreIndexed) {
  r::Condition offline;
  offline.kind = r::Condition::Kind::kStatus;
  offline.leaf = r::StatusLeaf{"*", DeviceStatus::kOffline};
  r::Condition mode;
  mode.kind = r::Condition::Kind::kMode;
  mode.leaf = r::ModeLeaf{"ALERT"};
  r::Condition night;
  night.kind = r::Condition::Kind::kTime;
  night.leaf = r::TimeLeaf{22 * 60, 6 * 60};
  r::RuleEngine engine(
      r::RuleSet{{rule("offline", offline), rule("alert", mode), rule("night", night)}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  ctx.statuses["mcu-cpp-01"] = DeviceStatus::kOffline;
  engine.on_status("mcu-cpp-01", ctx, SteadyTime{}, out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].rule, 0U);
  ctx.mode_value = "ALERT";
  engine.on_mode(ctx, SteadyTime{}, out);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[1].rule, 1U);
  ctx.minutes = 23 * 60;
  engine.on_tick({}, ctx, SteadyTime{}, out);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out[2].rule, 2U);
  EXPECT_EQ(engine.find("night"), 2U);
  EXPECT_EQ(engine.find("nope"), std::nullopt);
}

TEST(RuleEngine, SilenceLastsUntilNextFireAndClearAllEmitsCleared) {
  r::RuleEngine engine(r::RuleSet{{rule("hot", channel(kTemp, r::Op::kGe, 45.0)),
                                   rule("pend", channel(kTemp, r::Op::kGe, 45.0), seconds{10})}});
  FakeContext ctx;
  std::vector<r::RuleEffect> out;
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  engine.silence(0);
  EXPECT_TRUE(engine.silenced(0));
  ctx.values[kTemp] = 40.0;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  EXPECT_TRUE(engine.silenced(0));  // idle に戻っただけでは解けない
  ctx.values[kTemp] = 50.0;
  engine.on_channel(kTemp, ctx, SteadyTime{}, out);
  EXPECT_FALSE(engine.silenced(0));  // 再び発火した
  out.clear();
  engine.clear_all(out);
  EXPECT_EQ(kinds(out), std::vector<K>{K::kCleared});  // active だった1つだけ
  EXPECT_EQ(engine.state(0), r::RuleState::kIdle);
  EXPECT_EQ(engine.state(1), r::RuleState::kIdle);
  EXPECT_EQ(engine.next_deadline(), std::nullopt);
  EXPECT_EQ(r::to_string(r::RuleState::kCooldown), "cooldown");
}
