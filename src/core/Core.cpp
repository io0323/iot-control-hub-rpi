#include "core/Core.h"

#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::core {

namespace {

constexpr TimerKey kHeartbeatTimer{TimerKind::kUartHeartbeat};
constexpr TimerKey kLivenessTimer{TimerKind::kUartLiveness};
constexpr TimerKey kCmdSlotTimer{TimerKind::kUartCmdSlot};
constexpr TimerKey kStaleTimer{TimerKind::kStaleCheck};
constexpr std::chrono::seconds kStalePeriod{1};  // stale の判定は1秒周期（基本設計 第6章）
constexpr std::string_view kGwInitialMode = "NORMAL";  // システムモードの初期値（基本設計 第6章）

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
      dedup_(constants::kSeqDedupCapacity) {
  for (const auto& profile : config_.static_devices) {
    registry_.register_static(profile);
  }
  registry_.register_static(config_.uart_profile);
}

Outputs Core::start() {
  Outputs out;
  started_ = clock_.steady_now();
  out.uart.push_back(UartTx{bridge_.heartbeat_frame(uptime_ms())});
  timers_.schedule(started_ + constants::kHeartbeatPeriod, kHeartbeatTimer);
  timers_.schedule(started_ + kStalePeriod, kStaleTimer);
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
          publish_gw_state("online", out);
        } else {
          static_assert(std::is_same_v<T, UartLinkUp> || std::is_same_v<T, MqttDisconnected>);
        }
      },
      msg);
  return out;
}

bool Core::uart_online() const {
  const auto status = registry_.effective_status(config_.uart_profile.device_id);
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
  }
  schedule_uart_liveness();
  if (result.request_state) {
    queue_get_status();
  }
  send_pending_cmds(out);
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
    updated |= registry_.update_reading(device_id, channel, r.value("value", 0.0),
                                        r.value("quality", ""), clock_.steady_now());
  }
  if (updated && device_id != config_.uart_profile.device_id) {
    apply_liveness(liveness_.fresh_value(device_id), out);
  }
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
}

// ------------------------------------------------------------ UART の指令

void Core::queue_get_status() {
  Json cmd = Json::object();
  cmd["cmd_id"] = ids_.next_ulid();
  cmd["op"] = "get_status";
  cmd["priority"] = "auto";
  pending_payloads_.push_back(std::move(cmd));
}

void Core::send_pending_cmds(Outputs& out) {
  if (pending_cmds_.empty() && pending_payloads_.empty()) {
    return;
  }
  const auto now = clock_.steady_now();
  if (last_cmd_sent_ && now < *last_cmd_sent_ + constants::kUartCmdInterval) {
    timers_.schedule(*last_cmd_sent_ + constants::kUartCmdInterval, kCmdSlotTimer);
    return;
  }
  // 毎秒1件：1回に1件だけ送る
  while (!pending_cmds_.empty() || !pending_payloads_.empty()) {
    if (!pending_cmds_.empty()) {
      out.uart.push_back(UartTx{pending_cmds_.front()});
      pending_cmds_.pop_front();
    } else {
      const Json payload = std::move(pending_payloads_.front());
      pending_payloads_.pop_front();
      const auto cmd_ref = next_cmd_ref_;
      next_cmd_ref_ = static_cast<std::uint16_t>((next_cmd_ref_ + 1U) % constants::kCmdRefModulo);
      auto frame = bridge_.command_to_frame(payload, cmd_ref);
      if (std::holds_alternative<uart::CommandRejected>(frame)) {
        ++stats_.uart_cmd_rejected;
        continue;  // 送らなかったので枠は使わない
      }
      out.uart.push_back(UartTx{std::get<uart::Cmd>(frame)});
    }
    ++stats_.get_status_sent;
    last_cmd_sent_ = now;
    break;
  }
  if (!pending_cmds_.empty() || !pending_payloads_.empty()) {
    timers_.schedule(now + constants::kUartCmdInterval, kCmdSlotTimer);
  }
}

void Core::requeue_uart_cmd(const uart::Cmd& cmd) {
  pending_cmds_.push_front(cmd);
  if (!timers_.scheduled(kCmdSlotTimer)) {
    timers_.schedule(clock_.steady_now() + constants::kUartCmdInterval, kCmdSlotTimer);
  }
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
        send_pending_cmds(out);
        break;
      case TimerKind::kStaleCheck: {
        std::map<std::string, std::size_t> before;
        for (const auto& [id, info] : registry_.devices()) {
          for (const auto& [name, ch] : info.channels) {
            before[id] += ch.stale ? 1 : 0;
          }
        }
        for (const auto& change : liveness_.stale_check(now)) {
          apply_liveness(change, out);
        }
        for (const auto& [id, info] : registry_.devices()) {
          std::size_t stale = 0;
          for (const auto& [name, ch] : info.channels) {
            stale += ch.stale ? 1 : 0;
          }
          if (stale > before[id]) {
            stats_.stale_transitions += stale - before[id];
          }
        }
        timers_.schedule(now + kStalePeriod, kStaleTimer);
        break;
      }
    }
  }
  return out;
}

std::optional<SteadyTime> Core::next_deadline() {
  return timers_.next_deadline();
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
  payload["mode"] = kGwInitialMode;  // Phase 4 で SystemModeController の値に置き換える（#11）
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
