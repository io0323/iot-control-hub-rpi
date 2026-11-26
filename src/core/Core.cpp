#include "core/Core.h"

#include <set>

#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::core {

namespace {

constexpr TimerKey kHeartbeatTimer{TimerKind::kUartHeartbeat};
constexpr TimerKey kLivenessTimer{TimerKind::kUartLiveness};
constexpr TimerKey kStaleTimer{TimerKind::kStaleCheck};
constexpr TimerKey kQualityCountsTimer{TimerKind::kQualityCounts};
constexpr std::chrono::seconds kStalePeriod{1};
constexpr std::string_view kUnitCount = "count";  // stale の判定は1秒周期（基本設計 第6章）

std::string log_change(const device::LivenessChange& c) {
  const std::string from = c.from ? std::string{device::to_string(*c.from)} : "unseen";
  return "event=device_status device=" + c.device_id + " from=" + from +
         " to=" + std::string{device::to_string(c.to)};
}

}  // namespace

Core::Core(CoreConfig config, IClock& clock, IIdGenerator& ids)
    : config_(std::move(config)),
      clock_(clock),
      ids_(ids),
      bridge_(config_.uart_profile, ids, config_.device_id),
      started_(clock.steady_now()),
      liveness_(registry_),
      dedup_(constants::kSeqDedupCapacity),
      commands_(bridge_, clock, ids, config_.uart_profile.device_id,
                [this](const std::string& id) { return device_online(id); }),
      requests_(
          config_.requests, arbiter_, commands_,
          [this](const std::string& id) { return registry_.find(id) != nullptr; },
          [this](const std::string& id) { return device_online(id); },
          [this](const command::OwnerChange& change, command::CommandOutputs& cmd_out) {
            return apply_owner_change(change, cmd_out);
          }),
      rules_(config_.rules) {
  for (const auto& profile : config_.static_devices) {
    registry_.register_static(profile);
  }
  registry_.register_static(config_.uart_profile);
  setup_rules();
}

Outputs Core::start() {
  Outputs out;
  started_ = clock_.steady_now();
  out.uart.push_back(UartTx{bridge_.heartbeat_frame(uptime_ms())});
  timers_.schedule(started_ + constants::kHeartbeatPeriod, kHeartbeatTimer);
  timers_.schedule(started_ + kStalePeriod, kStaleTimer);
  if (const auto period = quality_counts_period()) {
    timers_.schedule(started_ + *period, kQualityCountsTimer);
  }
  std::vector<rules::RuleEffect> effects;
  rules_.evaluate_all(*rule_view_, started_, effects);  // 時刻帯・モードだけのルール
  apply_effects(effects, out);
  return out;
}

Outputs Core::step(const Inbound& msg) {
  Outputs out;
  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, UartReceived>) {
          on_uart(m, out);
        } else if constexpr (std::is_same_v<T, MqttReceived>) {
          on_mqtt(m, out);
        } else if constexpr (std::is_same_v<T, UartLinkDown>) {
          apply_liveness(
              liveness_.set_status(config_.uart_profile.device_id, device::DeviceStatus::kOffline),
              out);
          schedule_uart_liveness();
        } else if constexpr (std::is_same_v<T, MqttConnected>) {
          mqtt_connected_ = true;
          publish_gw_state("online", out);
        } else if constexpr (std::is_same_v<T, MqttDisconnected>) {
          mqtt_connected_ = false;
        } else if constexpr (std::is_same_v<T, Reload>) {
          on_reload(m, out);
        } else {
          static_assert(std::is_same_v<T, UartLinkUp>);
        }
      },
      msg);
  return out;
}

bool Core::uart_online() const {
  return device_online(config_.uart_profile.device_id);
}

bool Core::device_online(const std::string& device_id) const {
  const auto status = registry_.effective_status(device_id);
  return status == device::DeviceStatus::kOnline || status == device::DeviceStatus::kSuspect;
}

// ------------------------------------------------------------ UART

void Core::on_uart(const UartReceived& msg, Outputs& out) {
  ++stats_.uart_frames;
  const auto& uart_id = config_.uart_profile.device_id;
  if (std::holds_alternative<uart::Frame>(msg.decoded)) {
    // 復号できたフレームなら何でも生存確認を更新する。最初の受信までは数えない（open-questions #3）
    liveness_.uart_frame(uart_id, clock_.steady_now());
  }
  auto result = bridge_.receive(msg.decoded, msg.received_at);
  if (result.envelope) {
    const auto& env = *result.envelope;
    if (env.schema == "iot.v1.state" && env.payload["status"] == "online") {
      apply_liveness(liveness_.set_status(uart_id, device::DeviceStatus::kOnline), out);
    }
    // UART 由来の電文も台帳に入れる（重複・時刻ずれは判定しない：作品03 自身が作ったもの）
    apply_message(env.to_json(), env.kind(), msg.received_at, false, out);
    publish(env, out);
    if (env.kind() == kKindCmdAck) {
      command::CommandOutputs cmd_out;
      std::vector<command::RequestAck> acks;
      commands_.on_ack(env.payload, cmd_out);
      flush(cmd_out, acks, out);
    }
  }
  schedule_uart_liveness();
  if (result.request_state) {
    queue_get_status(out);
  }
}

void Core::schedule_uart_liveness() {
  if (const auto next = liveness_.uart_next_deadline()) {
    timers_.schedule(*next, kLivenessTimer);
  } else {
    timers_.cancel(kLivenessTimer);
  }
}

void Core::publish_uart_offline(Outputs& out) {
  const auto offline = bridge_.offline_state(clock_.system_now());
  if (!offline) {
    return;
  }
  ++stats_.uart_offline_published;
  publish(*offline, out);
}

// ------------------------------------------------------------ MQTT

void Core::on_mqtt(const MqttReceived& msg, Outputs& out) {
  ++stats_.mqtt_received;
  const auto& m = msg.message;
  const auto device_id = m.value("device_id", "");
  const auto boot_id = m.value("boot_id", "");
  if (device_id == config_.device_id || device_id == config_.uart_profile.device_id) {
    return;  // 自分の電文（mqtt-loop で捨てているはずだが念のため）
  }
  if (!quality::exempt_from_seq_checks(m)) {
    const auto seq = m.value("seq", std::uint32_t{0});
    if (dedup_.is_duplicate(device_id, boot_id, msg.kind, seq)) {
      ++stats_.mqtt_duplicates;
      return;  // 2通目以降を破棄（第5章）
    }
    stats_.mqtt_gaps += gaps_.observe(device_id, boot_id, msg.kind, seq);
  }
  Json message = m;
  bool skew = false;
  if (m.value("ts_source", "") == "device") {
    const auto ts = parse_ts(m.value("ts", ""));
    if (ts && quality::is_clock_skew(*ts, msg.received_at)) {
      // 内部では ts を受信時刻に置き換える。他デバイスの電文を発行し直すことはしない
      skew = true;
      ++stats_.mqtt_clock_skew;
      message["ts"] = format_ts(msg.received_at);
      message["ts_source"] = "gateway";
      if (skew_limiter_.allow(device_id, clock_.steady_now())) {
        publish_gw_event("CLOCK_SKEW", "warning",
                         Json{{"device_id", device_id}, {"ts", m.value("ts", "")}}, out);
      }
    }
  }
  if (msg.kind == kKindRequest || msg.kind == kKindCmdAck) {
    command::CommandOutputs cmd_out;
    std::vector<command::RequestAck> acks;
    if (msg.kind == kKindRequest) {
      requests_.handle(message["payload"], device_id, clock_.steady_now(), acks, cmd_out);
    } else {
      commands_.on_ack(message["payload"], cmd_out);
    }
    flush(cmd_out, acks, out);
    return;
  }
  apply_message(message, msg.kind, msg.received_at, !skew, out);
}

void Core::apply_message(const Json& message, const std::string& kind, SystemTime received_at,
                         bool check_late, Outputs& out) {
  const auto device_id = message.value("device_id", "");
  const auto& payload = message["payload"];
  if (kind == kKindState) {
    if (registry_.apply_state(device_id, message.value("boot_id", ""), payload)) {
      ++stats_.devices_registered;
      out.logs.push_back("event=device_registered device=" + device_id +
                         " channels=" + std::to_string(payload["channels"].size()));
    }
    if (device_id != config_.uart_profile.device_id) {
      const auto status = payload["status"] == "online" ? device::DeviceStatus::kOnline
                                                        : device::DeviceStatus::kOffline;
      apply_liveness(liveness_.set_status(device_id, status), out);
    }
    return;
  }
  if (kind != kKindTelemetry || !payload.contains("readings")) {
    return;  // event・cmd/ack は Phase 3・4 で使う
  }
  const auto* info = registry_.find(device_id);
  if (info == nullptr) {
    ++stats_.mqtt_unregistered;
    return;  // state で登録されるまで値は持たない（FR-01：state による自動登録）
  }
  const auto ts = parse_ts(message.value("ts", ""));
  bool updated = false;
  std::vector<rules::ChannelKey> changed;
  for (const auto& r : payload["readings"]) {
    const auto channel = r.value("channel", "");
    const auto ch = info->channels.find(channel);
    if (ch == info->channels.end()) {
      ++stats_.mqtt_unregistered;
      continue;
    }
    if (check_late && ts && quality::is_late(*ts, received_at, ch->second.period_ms)) {
      ++stats_.mqtt_late;
      continue;  // 遅延到着：最新値を更新しない（第5章）
    }
    if (registry_.update_reading(device_id, channel, r.value("value", 0.0), r.value("quality", ""),
                                 clock_.steady_now())) {
      updated = true;
      changed.emplace_back(device_id, channel);
    }
  }
  if (updated && device_id != config_.uart_profile.device_id) {
    apply_liveness(liveness_.fresh_value(device_id), out);
  }
  // 値が更新されたチャネルを参照するルールだけを評価する（基本設計 第6章）
  std::vector<rules::RuleEffect> effects;
  for (const auto& key : changed) {
    rules_.on_channel(key, *rule_view_, clock_.steady_now(), effects);
  }
  apply_effects(effects, out);
}

void Core::apply_liveness(const std::optional<device::LivenessChange>& change, Outputs& out) {
  if (!change) {
    return;
  }
  out.logs.push_back(log_change(*change));
  if (change->to == device::DeviceStatus::kOffline &&
      change->device_id == config_.uart_profile.device_id) {
    publish_uart_offline(out);  // bridged_by 付き・retain・seq を進めない（#36）
  }
  if (!change->event_code.empty()) {
    ++stats_.device_events;
    const bool online = change->event_code == "DEVICE_ONLINE";
    publish_gw_event(change->event_code, online ? "info" : "warning",
                     Json{{"device_id", change->device_id}}, out);
  }
  std::vector<rules::RuleEffect> effects;
  rules_.on_status(change->device_id, *rule_view_, clock_.steady_now(), effects);
  apply_effects(effects, out);
}

// ------------------------------------------------------------ 指令と要求

void Core::queue_get_status(Outputs& out) {
  // STATE を求める GET_STATUS：cmd_id を採番する普通の指令（priority auto、open-questions #2）。
  // STATE 待ちの間（まだ online でない）にも送る
  ++stats_.get_status_requested;
  command::CommandOutputs cmd_out;
  std::vector<command::RequestAck> acks;
  Json payload{{"op", "get_status"}, {"priority", "auto"}};
  commands_.submit(command::CommandManager::Submit{config_.uart_profile.device_id,
                                                   std::move(payload), "", false},
                   cmd_out);
  flush(cmd_out, acks, out);
}

std::optional<std::string> Core::apply_owner_change(const command::OwnerChange& change,
                                                    command::CommandOutputs& cmd_out) {
  if (!change.new_owner) {
    return std::nullopt;  // リースが切れればデバイスが自分で戻る。release は送らない
  }
  const auto& claim = *change.new_owner;
  const auto now = clock_.steady_now();
  const auto remaining = std::chrono::ceil<std::chrono::seconds>(claim.lease_until - now).count();
  if (remaining < constants::kLeaseMinS) {
    return std::nullopt;
  }
  Json payload = Json::object();
  payload["op"] = "set_actuator";
  payload["args"] = claim.args;
  payload["priority"] = std::string{command::to_string(claim.priority)};
  payload["lease_s"] = std::min<std::int64_t>(remaining, constants::kLeaseMaxS);
  if (!claim.origin.is_null()) {
    payload["origin"] = claim.origin;
  }
  return commands_.submit(
      command::CommandManager::Submit{change.key.device_id, std::move(payload), claim.owner, true},
      cmd_out);
}

void Core::flush(command::CommandOutputs& cmd_out, std::vector<command::RequestAck>& acks,
                 Outputs& out) {
  const auto now = clock_.steady_now();
  // 承認（#46）で変わったモード：SYSTEM_MODE_CHANGED を request/ack より先に出す
  auto changes = std::move(pending_mode_changes_);
  pending_mode_changes_.clear();
  for (const auto& change : changes) {
    apply_mode_change(change, cmd_out, out);
  }
  // 終端に達した指令 → request の最終結果・COMMAND_FAILED（処理中に増えることがある）
  for (std::size_t i = 0; i < cmd_out.finished.size(); ++i) {
    const auto cmd = cmd_out.finished[i];
    requests_.on_command_finished(cmd, now, acks, cmd_out);
    out.logs.push_back("event=cmd_finished cmd_id=" + cmd.cmd_id + " target=" + cmd.target +
                       " op=" + cmd.payload.value("op", "") +
                       " state=" + std::string{command::to_string(cmd.state)} +
                       " reason=" + (cmd.reason.empty() ? "-" : cmd.reason) +
                       " attempts=" + std::to_string(cmd.attempts));
    if (cmd.state == command::CmdState::kFailed || cmd.state == command::CmdState::kTimedOut) {
      ++stats_.command_failed_events;
      publish_gw_event("COMMAND_FAILED", "warning",
                       Json{{"cmd_id", cmd.cmd_id},
                            {"target", cmd.target},
                            {"op", cmd.payload.value("op", "")},
                            {"reason", cmd.reason}},
                       out);
    }
  }
  for (auto& cmd : cmd_out.uart) {
    out.uart.push_back(UartTx{std::move(cmd)});
  }
  for (auto& cmd : cmd_out.mqtt) {
    publish_gw(kKindCmd, std::move(cmd.payload), cmd.target, out);
  }
  for (auto& ack : acks) {
    publish_gw(kKindRequestAck, std::move(ack.payload), ack.requester, out);
  }
  cmd_out = {};
  acks.clear();
}

void Core::requeue_uart_cmd(const uart::Cmd& cmd) {
  commands_.requeue_uart_frame(cmd);
}

// ------------------------------------------------------------ タイマー

Outputs Core::on_timer(SteadyTime now) {
  Outputs out;
  for (const auto key : timers_.pop_due(now)) {
    switch (key.kind) {
      case TimerKind::kUartHeartbeat: {
        out.uart.push_back(UartTx{bridge_.heartbeat_frame(uptime_ms())});
        // 前の予定時刻から1秒ずつ（遅れても周期がずれていかないよう、過ぎた回は飛ばす）
        auto next = started_ + constants::kHeartbeatPeriod *
                                   ((now - started_) / constants::kHeartbeatPeriod + 1);
        timers_.schedule(next, kHeartbeatTimer);
        break;
      }
      case TimerKind::kUartLiveness:
        for (const auto& change : liveness_.uart_check(now)) {
          apply_liveness(change, out);
        }
        schedule_uart_liveness();
        break;
      case TimerKind::kUartCmdSlot:
        break;  // Phase 3 から CommandManager の期限で動く
      case TimerKind::kQualityCounts:
        publish_quality_counts(out);
        if (const auto period = quality_counts_period()) {
          timers_.schedule(now + *period, kQualityCountsTimer);
        }
        break;
      case TimerKind::kStaleCheck: {
        std::map<rules::ChannelKey, bool> before;
        for (const auto& [id, info] : registry_.devices()) {
          for (const auto& [name, ch] : info.channels) {
            before[{id, name}] = ch.stale;
          }
        }
        for (const auto& change : liveness_.stale_check(now)) {
          apply_liveness(change, out);
        }
        std::set<rules::ChannelKey> stale_changed;
        for (const auto& [id, info] : registry_.devices()) {
          for (const auto& [name, ch] : info.channels) {
            const auto it = before.find({id, name});
            const bool was = it != before.end() && it->second;
            if (ch.stale != was) {
              stale_changed.insert({id, name});
              stats_.stale_transitions += ch.stale ? 1 : 0;
            }
          }
        }
        // 1秒ごと：時刻帯を参照するルールと、stale の状態が変わったチャネルを参照するルール
        std::vector<rules::RuleEffect> effects;
        rules_.on_tick(stale_changed, *rule_view_, now, effects);
        apply_effects(effects, out);
        timers_.schedule(now + kStalePeriod, kStaleTimer);
        break;
      }
    }
  }
  std::vector<rules::RuleEffect> effects;
  rules_.on_timer(*rule_view_, now, effects);  // for・cooldown の期限
  apply_effects(effects, out);
  command::CommandOutputs cmd_out;
  std::vector<command::RequestAck> acks;
  commands_.on_timer(now, cmd_out);
  for (const auto& change : arbiter_.expire(now)) {
    apply_owner_change(change, cmd_out);
  }
  requests_.purge(now);
  flush(cmd_out, acks, out);
  return out;
}

std::optional<SteadyTime> Core::next_deadline() {
  std::optional<SteadyTime> next = timers_.next_deadline();
  for (const auto& at : {commands_.next_deadline(), arbiter_.next_expiry(), requests_.next_purge(),
                         rules_.next_deadline()}) {
    if (at && (!next || *at < *next)) {
      next = at;
    }
  }
  return next;
}

// ------------------------------------------------------------ 発行

void Core::publish(const Envelope& env, Outputs& out) const {
  const auto kind = env.kind();
  const auto info = require_kind(kind);
  out.mqtt.push_back(MqttTx{topic_for(config_.site, env.device_id, kind), env.to_json().dump(),
                            info.qos, info.retain, env.device_id, kind});
}

Envelope Core::gw_envelope(std::string_view kind, std::uint32_t seq, Json payload) {
  Envelope env;
  env.schema = std::string{require_kind(kind).schema};
  env.msg_id = ids_.next_ulid();
  env.device_id = config_.device_id;
  env.seq = seq;
  env.boot_id = config_.boot_id;
  env.ts = clock_.system_now();
  env.ts_source = "device";
  env.payload = std::move(payload);
  return env;
}

Json Core::gw_state_payload(const std::string& status) const {
  Json payload = Json::object();
  payload["status"] = status;
  payload["mode"] = mode_.mode();  // システムモード（#11、Phase 4：SystemModeController）
  Json channels = Json::array();
  for (const auto& c : config_.self_profile.channels) {
    channels.push_back(Json{{"channel", c.channel}, {"unit", c.unit}, {"period_ms", c.period_ms}});
  }
  Json actuators = Json::array();
  for (const auto& a : config_.self_profile.actuators) {
    actuators.push_back(
        Json{{"id", a.id}, {"min", a.min}, {"max", a.max}, {"max_on_s", a.max_on_s}});
  }
  payload["channels"] = std::move(channels);
  payload["actuators"] = std::move(actuators);
  payload["fw_version"] = config_.fw_version;
  return payload;
}

void Core::publish_gw_state(const std::string& status, Outputs& out) {
  const std::string kind{kKindState};
  publish(gw_envelope(kind, gw_seq_.next(kind), gw_state_payload(status)), out);
}

void Core::publish_gw(std::string_view kind, Json payload, const std::string& topic_device,
                      Outputs& out) {
  const std::string k{kind};
  const auto env = gw_envelope(k, gw_seq_.next(k), std::move(payload));
  const auto info = require_kind(k);
  out.mqtt.push_back(MqttTx{topic_for(config_.site, topic_device, k), env.to_json().dump(),
                            info.qos, info.retain, env.device_id, k});
}

void Core::publish_gw_event(const std::string& code, const std::string& severity, Json detail,
                            Outputs& out) {
  const std::string kind{kKindEvent};
  Json payload{{"code", code}, {"severity", severity}, {"detail", std::move(detail)}};
  publish(gw_envelope(kind, gw_seq_.next(kind), std::move(payload)), out);
}

MqttTx Core::gw_offline_state() {
  Outputs out;
  const std::string kind{kKindState};
  publish(gw_envelope(kind, gw_seq_.peek(kind), gw_state_payload("offline")), out);
  return out.mqtt.front();
}

std::optional<Millis> Core::quality_counts_period() const {
  std::optional<Millis> period;
  for (const auto& c : config_.self_profile.channels) {
    if (c.unit == kUnitCount && c.period_ms > 0 && (!period || Millis{c.period_ms} < *period)) {
      period = Millis{c.period_ms};
    }
  }
  return period;
}

Json Core::quality_counts() const {
  const ExternalCounts external = external_counts_ ? external_counts_() : ExternalCounts{};
  std::uint64_t uart_discarded = 0;
  for (const auto& [reason, count] : bridge_.discards()) {
    if (reason != uart::kNotConverted) {
      uart_discarded += count;  // not_converted（DIAG など）は含めない（第5章）
    }
  }
  const std::map<std::string, std::uint64_t, std::less<>> values{
      {"mqtt_rejected", external.mqtt_rejected}, {"mqtt_duplicates", stats_.mqtt_duplicates},
      {"mqtt_gaps", stats_.mqtt_gaps},           {"mqtt_clock_skew", stats_.mqtt_clock_skew},
      {"mqtt_late", stats_.mqtt_late},           {"stale_transitions", stats_.stale_transitions},
      {"uart_discarded", uart_discarded},        {"queue_dropped", external.queue_dropped},
  };
  Json readings = Json::array();
  for (const auto& c : config_.self_profile.channels) {
    if (c.unit != kUnitCount) {
      continue;
    }
    const auto it = values.find(c.channel);
    readings.push_back(Json{{"channel", c.channel},
                            {"value", it == values.end() ? 0 : it->second},
                            {"unit", c.unit},
                            {"quality", "good"}});
  }
  return Json{{"readings", std::move(readings)}};
}

void Core::publish_quality_counts(Outputs& out) {
  const std::string kind{kKindTelemetry};
  publish(gw_envelope(kind, gw_seq_.next(kind), quality_counts()), out);
}

Outputs Core::shutdown() {
  Outputs out;
  out.mqtt.push_back(gw_offline_state());
  return out;
}

std::uint32_t Core::uptime_ms() const {
  const auto elapsed = std::chrono::duration_cast<Millis>(clock_.steady_now() - started_).count();
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(elapsed) & constants::kUint32Max);
}

}  // namespace gw::core
