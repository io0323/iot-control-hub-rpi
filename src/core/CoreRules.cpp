// Core のうち、ルールとシステムモードにかかわる部分（Phase 4）。
//
// - RuleEngine の効果をアクションにする：command（調停して指令。hold は CommandManager::keep）、
//   set_system_mode、notify（RULE_FIRED の重大度と message。v0.2.3 #47）、reconnect（#32）
// - 解除：RULE_CLEARED、hold の指令は更新を止めて release（待ちがあれば引き継ぎ）
// - 不明：更新を止める。真に戻ったら hold の指令を送り直す（#30）
// - モード：SYSTEM_MODE_CHANGED（v0.2.3 #48）と gw-01 の state。EMERGENCY を抜けたら止めていた
//   auto・schedule のルールの hold を送り直す（#34）
// - 承認（#46）、手動 release でルールを黙らせる（#26）、SIGHUP の再読込（#38・#39）
#include <cmath>

#include "core/Core.h"
#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::core {

namespace {

constexpr std::string_view kRuleOwnerPrefix = "rule:";
constexpr std::string_view kSetActuator = "set_actuator";
constexpr std::string_view kQualityGood = "good";
constexpr int kTokyoOffsetMin = 9 * 60;  // gateway.yaml の timezone の既定（Asia/Tokyo）
constexpr std::int64_t kMinutesPerDay = 24 * 60;
constexpr std::int64_t kMsPerMinute = 60'000;

std::string actuator_of(const rules::CommandAction& a) {
  return a.args.contains("id") && a.args["id"].is_string() ? a.args["id"].get<std::string>() : "";
}

}  // namespace

// RuleEngine に答える値（open-questions #30：使えない値は nullopt＝不明）
class Core::RuleView final : public rules::RuleContext {
 public:
  explicit RuleView(const Core& core) : core_(core) {}

  [[nodiscard]] std::optional<double> value(const std::string& device,
                                            const std::string& channel) const override {
    const auto* info = core_.registry_.find(device);
    if (info == nullptr) {
      return std::nullopt;
    }
    const auto it = info->channels.find(channel);
    if (it == info->channels.end()) {
      return std::nullopt;
    }
    const auto& ch = it->second;
    if (ch.stale || ch.quality != kQualityGood) {
      return std::nullopt;  // stale・quality が good でない値では発火しない
    }
    return ch.value;
  }

  [[nodiscard]] bool status_is(const std::string& device,
                               device::DeviceStatus status) const override {
    if (device == rules::kAnyDevice) {
      for (const auto& [id, info] : core_.registry_.devices()) {
        if (core_.registry_.effective_status(id) == status) {
          return true;
        }
      }
      return false;
    }
    return core_.registry_.effective_status(device) == status;
  }

  [[nodiscard]] std::string mode() const override { return core_.mode_.mode(); }
  [[nodiscard]] int local_minutes() const override { return core_.local_minutes(); }

 private:
  const Core& core_;
};

Core::~Core() = default;

void Core::setup_rules() {
  rule_view_ = std::make_unique<RuleView>(*this);
  commands_.set_renew_guard(
      [this](const std::string& owner, const std::string& target, const Json& payload) {
        return renew_allowed(owner, target, payload);
      });
  requests_.set_system_hooks(command::RequestHandler::SystemHooks{
      config_.device_id,
      [this] { return mode_.emergency(); },
      [this] { return approve_emergency_release(); },
      [this](const std::string& owner) {
        if (!owner.starts_with(kRuleOwnerPrefix)) {
          return;
        }
        if (const auto i = rules_.find(owner.substr(kRuleOwnerPrefix.size()))) {
          rules_.silence(*i);  // 再び発火するまで指令もリース更新もしない（#26）
          commands_.stop_keeping(owner);
        }
      }});
}

int Core::local_minutes() const {
  const auto now = clock_.system_now();
  const int offset = utc_offset_ ? utc_offset_(now) : kTokyoOffsetMin;
  const auto minutes = now.time_since_epoch().count() / kMsPerMinute + offset;
  return static_cast<int>(((minutes % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay);
}

bool Core::rule_active(const std::string& rule_id) const {
  const auto i = rules_.find(rule_id);
  return i && rules_.state(*i) == rules::RuleState::kActive;
}

bool Core::rule_idle(const std::string& rule_id) const {
  const auto i = rules_.find(rule_id);
  return !i || rules_.state(*i) == rules::RuleState::kIdle;  // 再読込で消えたルールは idle とみなす
}

// ------------------------------------------------------------ 効果

void Core::apply_effects(std::vector<rules::RuleEffect>& effects, Outputs& out) {
  if (effects.empty()) {
    return;
  }
  command::CommandOutputs cmd_out;
  std::vector<command::RequestAck> acks;
  for (const auto& e : effects) {
    const auto& rule = rules_.rules().at(e.rule);
    switch (e.kind) {
      case rules::RuleEffect::Kind::kFired:
        on_rule_fired(e.rule, cmd_out, out);
        break;
      case rules::RuleEffect::Kind::kCleared:
        on_rule_cleared(e.rule, cmd_out, out);
        break;
      case rules::RuleEffect::Kind::kUnknown:
        // 参照する値が不明：新しい指令を出さず、リースの更新も止める（第6章「時間条件と値の鮮度」）
        stop_rule_commands(e.rule);
        out.logs.push_back("event=rule_unknown rule=" + rule.id);
        break;
      case rules::RuleEffect::Kind::kResumed:
        out.logs.push_back("event=rule_resumed rule=" + rule.id);
        send_rule_commands(e.rule, true, cmd_out, out);
        break;
    }
  }
  effects.clear();
  flush(cmd_out, acks, out);
}

void Core::on_rule_fired(std::size_t i, command::CommandOutputs& cmd_out, Outputs& out) {
  const auto& rule = rules_.rules().at(i);
  ++stats_.rules_fired;
  // RULE_FIRED：重大度は notify の level、detail は {rule_id, message?}（v0.2.3 #47）
  Json detail{{"rule_id", rule.id}};
  std::string severity = "info";
  if (const auto* notify = rule.notify()) {
    severity = notify->level;
    if (notify->message) {
      detail["message"] = *notify->message;
    }
  }
  publish_gw_event("RULE_FIRED", severity, std::move(detail), out);
  out.logs.push_back("event=rule_fired rule=" + rule.id + " priority=" +
                     std::string{command::to_string(rule.priority)});
  for (const auto& action : rule.then) {
    if (const auto* m = std::get_if<rules::ModeAction>(&action)) {
      if (const auto change = mode_.raise(m->mode, rule.id)) {
        apply_mode_change(*change, cmd_out, out);
      }
    } else if (std::holds_alternative<rules::ReconnectAction>(action)) {
      // 作品03 が持つ接続だけを開き直す。MQTT のデバイスには何もしない（#32）
      // UART のデバイスが offline と分かっているとき（まだ観測していないなら開き直さない）
      const bool uart = registry_.effective_status(config_.uart_profile.device_id) ==
                        device::DeviceStatus::kOffline;
      const bool mqtt = !mqtt_connected_;
      out.reconnect_uart |= uart;
      out.reconnect_mqtt |= mqtt;
      std::string others;
      for (const auto& [id, info] : registry_.devices()) {
        if (id != config_.uart_profile.device_id &&
            registry_.effective_status(id) == device::DeviceStatus::kOffline) {
          others += (others.empty() ? "" : ",") + id;
        }
      }
      out.logs.push_back("event=rule_reconnect rule=" + rule.id +
                         " uart=" + (uart ? "true" : "false") +
                         " mqtt=" + (mqtt ? "true" : "false") +
                         " mqtt_devices_offline=" + (others.empty() ? "-" : others));
    }
  }
  send_rule_commands(i, false, cmd_out, out);
}

void Core::send_rule_commands(std::size_t i, bool hold_only, command::CommandOutputs& cmd_out,
                              Outputs& out) {
  const auto& rule = rules_.rules().at(i);
  if (rules_.silenced(i)) {
    return;  // 手動の release で外された（#26）
  }
  if (mode_.suppressed(rule.priority)) {
    out.logs.push_back("event=rule_command_suppressed rule=" + rule.id + " mode=EMERGENCY");
    return;  // EMERGENCY では auto・schedule のルールの指令を止める（#34）
  }
  const auto now = clock_.steady_now();
  const auto owner = rule.owner();
  for (const auto& action : rule.then) {
    const auto* a = std::get_if<rules::CommandAction>(&action);
    if (a == nullptr || (hold_only && !a->hold)) {
      continue;
    }
    Json payload = Json::object();
    payload["op"] = a->op;
    if (!a->args.empty()) {
      payload["args"] = a->args;
    }
    payload["priority"] = std::string{command::to_string(rule.priority)};
    const Json origin{{"kind", "rule"}, {"ref", rule.id}};
    if (a->op == kSetActuator) {
      payload["lease_s"] = a->lease_s;
      const command::ActuatorKey key{a->device, actuator_of(*a)};
      const auto result = arbiter_.claim(
          key,
          command::Claim{owner, rule.priority, a->args, origin,
                         now + std::chrono::seconds{a->lease_s}, 0},
          now);
      if (!result.accepted) {
        ++stats_.rule_commands_rejected;  // 上位が所有している：この回は送らない（#33）
        out.logs.push_back("event=rule_command_lower_priority rule=" + rule.id +
                           " target=" + a->device + " actuator=" + key.actuator);
        continue;
      }
    }
    payload["origin"] = origin;
    if (a->hold) {
      (void)commands_.keep(owner, a->device, std::move(payload), cmd_out);
    } else {
      (void)commands_.submit(
          command::CommandManager::Submit{a->device, std::move(payload), owner, true}, cmd_out);
    }
  }
}

void Core::stop_rule_commands(std::size_t i) {
  commands_.stop_keeping(rules_.rules().at(i).owner());
}

void Core::on_rule_cleared(std::size_t i, command::CommandOutputs& cmd_out, Outputs& out) {
  const auto& rule = rules_.rules().at(i);
  ++stats_.rules_cleared;
  publish_gw_event("RULE_CLEARED", "info", Json{{"rule_id", rule.id}}, out);
  out.logs.push_back("event=rule_cleared rule=" + rule.id);
  const auto owner = rule.owner();
  commands_.stop_keeping(owner);
  const auto now = clock_.steady_now();
  for (const auto& action : rule.then) {
    const auto* a = std::get_if<rules::CommandAction>(&action);
    if (a == nullptr || !a->hold || a->op != kSetActuator) {
      continue;
    }
    const command::ActuatorKey key{a->device, actuator_of(*a)};
    const auto change = arbiter_.release(key, owner, now);
    if (!change) {
      continue;  // 所有していない（奪われた・手動で外された）
    }
    if (change->new_owner) {
      (void)apply_owner_change(*change, cmd_out);  // 待ちが引き継ぐ（#24）
      continue;
    }
    // 解除時に release を送る（基本設計 第6章）
    Json payload{{"op", "release"},
                 {"args", Json{{"id", key.actuator}}},
                 {"priority", std::string{command::to_string(rule.priority)}},
                 {"origin", Json{{"kind", "rule"}, {"ref", rule.id}}}};
    (void)commands_.submit(command::CommandManager::Submit{a->device, std::move(payload), owner, true},
                           cmd_out);
  }
  if (const auto change =
          mode_.rule_cleared(rule.id, [this](const std::string& id) { return rule_active(id); })) {
    apply_mode_change(*change, cmd_out, out);
  }
}

bool Core::renew_allowed(const std::string& owner, const std::string& target,
                         const Json& payload) {
  if (!owner.starts_with(kRuleOwnerPrefix)) {
    return true;
  }
  const auto i = rules_.find(owner.substr(kRuleOwnerPrefix.size()));
  if (!i || rules_.state(*i) != rules::RuleState::kActive || rules_.unknown(*i) ||
      rules_.silenced(*i)) {
    return false;
  }
  const auto& rule = rules_.rules().at(*i);
  if (mode_.suppressed(rule.priority)) {
    return false;  // EMERGENCY では auto・schedule のルールのリースを更新しない（#34）
  }
  if (payload.value("op", "") != kSetActuator || !payload.contains("args")) {
    return true;
  }
  // 自分が所有者か、所有者がいないときだけ更新し、調停の期限を延ばす（#33）。
  // 上位が所有している間は、その制御を更新で上書きしない
  const auto& args = payload["args"];
  const command::ActuatorKey key{target, args.value("id", "")};
  if (const auto* current = arbiter_.owner(key); current != nullptr && current->owner != owner) {
    return false;
  }
  const auto now = clock_.steady_now();
  const auto lease_s = payload.value("lease_s", constants::kLeaseDefaultS);
  return arbiter_
      .claim(key,
             command::Claim{owner, rule.priority, args,
                            payload.contains("origin") ? payload["origin"] : Json{},
                            now + std::chrono::seconds{lease_s}, 0},
             now)
      .accepted;
}

// ------------------------------------------------------------ モード

void Core::apply_mode_change(const mode::ModeChange& change, command::CommandOutputs& cmd_out,
                             Outputs& out) {
  ++stats_.mode_changes;
  // SYSTEM_MODE_CHANGED：重大度は遷移先、detail は {from, to, reason}（v0.2.3 #48）
  publish_gw_event("SYSTEM_MODE_CHANGED", std::string{mode::severity_of(change.to)},
                   Json{{"from", change.from}, {"to", change.to}, {"reason", change.reason}}, out);
  publish_gw_state("online", out);  // state の mode を更新する（#11）
  out.logs.push_back("event=system_mode from=" + change.from + " to=" + change.to +
                     " reason=" + change.reason);
  if (change.from == mode::kEmergency) {
    // 止めていた auto・schedule のルールの hold を送り直して更新を再開する（#34）
    for (std::size_t i = 0; i < rules_.rules().size(); ++i) {
      const auto& rule = rules_.rules()[i];
      if (rules_.state(i) == rules::RuleState::kActive && !rules_.unknown(i) &&
          (rule.priority == command::Priority::kAuto ||
           rule.priority == command::Priority::kSchedule)) {
        send_rule_commands(i, true, cmd_out, out);
      }
    }
  }
  std::vector<rules::RuleEffect> effects;
  rules_.on_mode(*rule_view_, clock_.steady_now(), effects);
  apply_effects(effects, out);
}

std::optional<std::string> Core::approve_emergency_release() {
  if (mode_.mode() != mode::kNormal) {
    // 条件の回復：EMERGENCY は入れたルールがすべて idle、ALERT は active でない（#36・#29）
    const bool emergency = mode_.emergency();
    for (const auto& id : mode_.entered_by()) {
      if (emergency ? !rule_idle(id) : rule_active(id)) {
        return std::nullopt;  // unsafe
      }
    }
    if (auto change = mode_.approve(std::string{mode::kApprovedByRequest})) {
      pending_mode_changes_.push_back(std::move(*change));
    }
    // まだ active の ALERT のルールがあれば、続けて ALERT に入る（#36）
    const auto alert_rules = mode_.alert_rules();
    for (const auto& id : alert_rules) {
      if (rule_active(id)) {
        if (auto change = mode_.raise(mode::kAlert, id)) {
          pending_mode_changes_.push_back(std::move(*change));
        }
        break;
      }
    }
  }
  return ids_.next_ulid();  // 承認の処理を表す ULID（request/ack の cmd_id）
}

// ------------------------------------------------------------ 再読込

void Core::on_reload(const Reload& msg, Outputs& out) {
  // 入れ替える前に、active のルールの解除処理（RULE_CLEARED と release）をする（#39）
  std::vector<rules::RuleEffect> effects;
  rules_.clear_all(effects);
  apply_effects(effects, out);
  rules_ = rules::RuleEngine(msg.rules);
  requests_.set_rules(msg.requests);
  // devices.yaml：静的登録を足す・入れ替える。消えたデバイスは再起動まで残す（#38）
  std::set<std::string> listed;
  for (const auto& profile : msg.devices) {
    listed.insert(profile.device_id);
    if (profile.device_id == config_.uart_profile.device_id) {
      continue;  // UART のデバイスのプロファイルの変更は再起動が要る
    }
    registry_.register_static(profile);
  }
  for (const auto& [id, info] : registry_.devices()) {
    if (info.is_static && !listed.contains(id)) {
      out.logs.push_back("event=reload_device_kept device=" + id + " reason=restart_required");
    }
  }
  ++stats_.reloads;
  out.logs.push_back("event=reloaded rules=" + std::to_string(rules_.rules().size()) +
                     " devices=" + std::to_string(msg.devices.size()) +
                     " requests=" + std::to_string(msg.requests.allow.size()));
  rules_.evaluate_all(*rule_view_, clock_.steady_now(), effects);
  apply_effects(effects, out);
}

}  // namespace gw::core
