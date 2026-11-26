#include "core/rules/RuleEngine.h"

#include <algorithm>
#include <array>

#include "core/common/Optional.h"

namespace gw::core::rules {

namespace {

constexpr std::array<std::pair<Op, std::string_view>, 6> kOpNames{{
    {Op::kGe, ">="},
    {Op::kGt, ">"},
    {Op::kLe, "<="},
    {Op::kLt, "<"},
    {Op::kEq, "=="},
    {Op::kNe, "!="},
}};
constexpr int kMinutesPerDay = 24 * 60;

bool compare(Op op, double value, double threshold) {
  switch (op) {
    case Op::kGe:
      return value >= threshold;
    case Op::kGt:
      return value > threshold;
    case Op::kLe:
      return value <= threshold;
    case Op::kLt:
      return value < threshold;
    case Op::kEq:
      return value == threshold;  // NOLINT(clang-diagnostic-float-equal)
    case Op::kNe:
      return value != threshold;  // NOLINT(clang-diagnostic-float-equal)
  }
  return false;
}

// active の間のヒステリシス（open-questions #31）：解除は しきい値∓値 を越えたとき
bool held(Op op, double value, double threshold, double hysteresis) {
  switch (op) {
    case Op::kGe:
    case Op::kGt:
      return !(value < threshold - hysteresis);
    case Op::kLe:
    case Op::kLt:
      return !(value > threshold + hysteresis);
    case Op::kEq:
    case Op::kNe:
      return compare(op, value, threshold);
  }
  return false;
}

Tri from_bool(bool b) {
  return b ? Tri::kTrue : Tri::kFalse;
}

bool in_window(const TimeLeaf& t, int minutes) {
  if (t.from_min == t.to_min) {
    return true;  // 終日
  }
  if (t.from_min < t.to_min) {
    return minutes >= t.from_min && minutes < t.to_min;
  }
  return minutes >= t.from_min || minutes < t.to_min;  // 日をまたぐ
}

}  // namespace

std::optional<Op> parse_op(std::string_view text) {
  for (const auto& [op, name] : kOpNames) {
    if (name == text) {
      return op;
    }
  }
  return std::nullopt;
}

std::string_view to_string(Op op) {
  for (const auto& [o, name] : kOpNames) {
    if (o == op) {
      return name;
    }
  }
  return "?";
}

std::string_view to_string(RuleState s) {
  switch (s) {
    case RuleState::kIdle:
      return "idle";
    case RuleState::kPending:
      return "pending";
    case RuleState::kActive:
      return "active";
    case RuleState::kCooldown:
      return "cooldown";
  }
  return "?";
}

const NotifyAction* Rule::notify() const {
  for (const auto& a : then) {
    if (const auto* n = std::get_if<NotifyAction>(&a)) {
      return n;
    }
  }
  return nullptr;
}

std::optional<std::string> Rule::sets_mode() const {
  std::optional<std::string> mode;
  for (const auto& a : then) {
    if (const auto* m = std::get_if<ModeAction>(&a)) {
      mode = m->mode;
    }
  }
  return mode;
}

RuleEngine::RuleEngine(RuleSet set) : rules_(std::move(set.rules)), runtime_(rules_.size()) {
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    index(i, rules_[i].when);
  }
}

void RuleEngine::index(std::size_t i, const Condition& c) {
  if (const auto* ch = std::get_if<ChannelLeaf>(&c.leaf)) {
    by_channel_[{ch->device, ch->channel}].insert(i);
  } else if (const auto* st = std::get_if<StatusLeaf>(&c.leaf)) {
    by_status_[st->device].insert(i);
  } else if (std::holds_alternative<ModeLeaf>(c.leaf)) {
    by_mode_.insert(i);
  } else if (std::holds_alternative<TimeLeaf>(c.leaf)) {
    by_time_.insert(i);
  }
  for (const auto& child : c.children) {
    index(i, child);
  }
}

std::optional<std::size_t> RuleEngine::find(const std::string& id) const {
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    if (rules_[i].id == id) {
      return i;
    }
  }
  return std::nullopt;
}

Tri RuleEngine::eval(const Condition& c, const Rule& rule, bool active, const RuleContext& ctx) {
  switch (c.kind) {
    case Condition::Kind::kAll: {
      bool unknown = false;
      for (const auto& child : c.children) {
        const auto r = eval(child, rule, active, ctx);
        if (r == Tri::kFalse) {
          return Tri::kFalse;
        }
        unknown |= r == Tri::kUnknown;
      }
      return unknown ? Tri::kUnknown : Tri::kTrue;
    }
    case Condition::Kind::kAny: {
      bool unknown = false;
      for (const auto& child : c.children) {
        const auto r = eval(child, rule, active, ctx);
        if (r == Tri::kTrue) {
          return Tri::kTrue;
        }
        unknown |= r == Tri::kUnknown;
      }
      return unknown ? Tri::kUnknown : Tri::kFalse;
    }
    case Condition::Kind::kNot: {
      if (c.children.empty()) {
        return Tri::kUnknown;
      }
      const auto r = eval(c.children.front(), rule, active, ctx);
      return r == Tri::kUnknown ? Tri::kUnknown : from_bool(r == Tri::kFalse);
    }
    case Condition::Kind::kChannel: {
      const auto& leaf = std::get<ChannelLeaf>(c.leaf);
      const auto value = ctx.value(leaf.device, leaf.channel);
      const auto* v = opt_ptr(value);
      if (v == nullptr) {
        return Tri::kUnknown;
      }
      const auto* h = opt_ptr(rule.hysteresis);
      if (active && h != nullptr) {
        return from_bool(held(leaf.op, *v, leaf.value, *h));
      }
      return from_bool(compare(leaf.op, *v, leaf.value));
    }
    case Condition::Kind::kStatus: {
      const auto& leaf = std::get<StatusLeaf>(c.leaf);
      return from_bool(ctx.status_is(leaf.device, leaf.status));
    }
    case Condition::Kind::kMode:
      return from_bool(ctx.mode() == std::get<ModeLeaf>(c.leaf).mode);
    case Condition::Kind::kTime:
      return from_bool(
          in_window(std::get<TimeLeaf>(c.leaf), ((ctx.local_minutes() % kMinutesPerDay) +
                                                 kMinutesPerDay) % kMinutesPerDay));
  }
  return Tri::kUnknown;
}

void RuleEngine::fire(std::size_t i, std::vector<RuleEffect>& out) {
  auto& rt = runtime_[i];
  rt.state = RuleState::kActive;
  rt.deadline.reset();
  rt.unknown = false;
  rt.silenced = false;
  out.push_back(RuleEffect{RuleEffect::Kind::kFired, i});
}

void RuleEngine::evaluate(std::size_t i, const RuleContext& ctx, SteadyTime now,
                          std::vector<RuleEffect>& out) {
  auto& rt = runtime_[i];
  const auto& rule = rules_[i];
  if (rt.state == RuleState::kCooldown) {
    return;  // 期限まで評価しない
  }
  ++evaluations_;
  const auto r = eval(rule.when, rule, rt.state == RuleState::kActive, ctx);
  switch (rt.state) {
    case RuleState::kIdle:
      if (r == Tri::kTrue) {
        if (rule.for_duration.count() <= 0) {
          fire(i, out);
        } else {
          rt.state = RuleState::kPending;
          rt.deadline = now + rule.for_duration;
        }
      }
      break;
    case RuleState::kPending:
      if (r != Tri::kTrue) {
        rt.state = RuleState::kIdle;  // 不成立・不明（値の途絶）なら取り消す
        rt.deadline.reset();
      }
      break;
    case RuleState::kActive:
      if (r == Tri::kFalse) {
        out.push_back(RuleEffect{RuleEffect::Kind::kCleared, i});
        rt.unknown = false;
        if (rule.cooldown.count() > 0) {
          rt.state = RuleState::kCooldown;
          rt.deadline = now + rule.cooldown;
        } else {
          rt.state = RuleState::kIdle;
          rt.deadline.reset();
        }
      } else if (r == Tri::kUnknown && !rt.unknown) {
        rt.unknown = true;
        out.push_back(RuleEffect{RuleEffect::Kind::kUnknown, i});
      } else if (r == Tri::kTrue && rt.unknown) {
        rt.unknown = false;
        out.push_back(RuleEffect{RuleEffect::Kind::kResumed, i});
      }
      break;
    case RuleState::kCooldown:
      break;
  }
}

void RuleEngine::on_channel(const ChannelKey& key, const RuleContext& ctx, SteadyTime now,
                            std::vector<RuleEffect>& out) {
  const auto it = by_channel_.find(key);
  if (it == by_channel_.end()) {
    return;
  }
  for (const auto i : it->second) {
    evaluate(i, ctx, now, out);
  }
}

void RuleEngine::on_status(const std::string& device, const RuleContext& ctx, SteadyTime now,
                           std::vector<RuleEffect>& out) {
  std::set<std::size_t> targets;
  for (const auto& name : {device, std::string{kAnyDevice}}) {
    if (const auto it = by_status_.find(name); it != by_status_.end()) {
      targets.insert(it->second.begin(), it->second.end());
    }
  }
  for (const auto i : targets) {
    evaluate(i, ctx, now, out);
  }
}

void RuleEngine::on_mode(const RuleContext& ctx, SteadyTime now, std::vector<RuleEffect>& out) {
  for (const auto i : by_mode_) {
    evaluate(i, ctx, now, out);
  }
}

void RuleEngine::on_tick(const std::set<ChannelKey>& stale_changed, const RuleContext& ctx,
                         SteadyTime now, std::vector<RuleEffect>& out) {
  std::set<std::size_t> targets = by_time_;
  for (const auto& key : stale_changed) {
    if (const auto it = by_channel_.find(key); it != by_channel_.end()) {
      targets.insert(it->second.begin(), it->second.end());
    }
  }
  for (const auto i : targets) {
    evaluate(i, ctx, now, out);
  }
}

void RuleEngine::on_timer(const RuleContext& ctx, SteadyTime now, std::vector<RuleEffect>& out) {
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    auto& rt = runtime_[i];
    const auto* deadline = opt_ptr(rt.deadline);
    if (deadline == nullptr || now < *deadline) {
      continue;
    }
    if (rt.state == RuleState::kPending) {
      // for の経過：その時点の最新値で評価し直し、成立していれば発火する
      ++evaluations_;
      if (eval(rules_[i].when, rules_[i], false, ctx) == Tri::kTrue) {
        fire(i, out);
      } else {
        rt.state = RuleState::kIdle;
        rt.deadline.reset();
      }
    } else if (rt.state == RuleState::kCooldown) {
      rt.state = RuleState::kIdle;
      rt.deadline.reset();
      evaluate(i, ctx, now, out);
    }
  }
}

void RuleEngine::evaluate_all(const RuleContext& ctx, SteadyTime now,
                              std::vector<RuleEffect>& out) {
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    evaluate(i, ctx, now, out);
  }
}

void RuleEngine::clear_all(std::vector<RuleEffect>& out) {
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    auto& rt = runtime_[i];
    if (rt.state == RuleState::kActive) {
      out.push_back(RuleEffect{RuleEffect::Kind::kCleared, i});
    }
    rt = Runtime{};
  }
}

std::optional<SteadyTime> RuleEngine::next_deadline() const {
  std::optional<SteadyTime> next;
  for (const auto& rt : runtime_) {
    const auto* d = opt_ptr(rt.deadline);
    const auto* n = opt_ptr(next);
    if (d != nullptr && (n == nullptr || *d < *n)) {
      next = *d;
    }
  }
  return next;
}

}  // namespace gw::core::rules
