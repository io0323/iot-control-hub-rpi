#include "core/Core.h"

#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::core {

namespace {

constexpr TimerKey kHeartbeatTimer{TimerKind::kUartHeartbeat};
constexpr TimerKey kLivenessTimer{TimerKind::kUartLiveness};
constexpr TimerKey kCmdSlotTimer{TimerKind::kUartCmdSlot};
constexpr std::string_view kGwInitialMode = "NORMAL";  // システムモードの初期値（基本設計 第6章）

}  // namespace

Core::Core(CoreConfig config, IClock& clock, IIdGenerator& ids)
    : config_(std::move(config)),
      clock_(clock),
      ids_(ids),
      bridge_(config_.uart_profile, ids, config_.device_id),
      started_(clock.steady_now()) {
}

Outputs Core::start() {
  Outputs out;
  started_ = clock_.steady_now();
  out.uart.push_back(UartTx{bridge_.heartbeat_frame(uptime_ms())});
  timers_.schedule(started_ + constants::kHeartbeatPeriod, kHeartbeatTimer);
  return out;
}

Outputs Core::step(const Inbound& msg) {
  Outputs out;
  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, UartReceived>) {
          on_uart(m, out);
        } else if constexpr (std::is_same_v<T, UartLinkDown>) {
          timers_.cancel(kLivenessTimer);
          publish_uart_offline(out);
        } else if constexpr (std::is_same_v<T, MqttConnected>) {
          publish_gw_state("online", out);
        } else {
          static_assert(std::is_same_v<T, UartLinkUp> || std::is_same_v<T, MqttDisconnected>);
        }
      },
      msg);
  return out;
}

void Core::on_uart(const UartReceived& msg, Outputs& out) {
  ++stats_.uart_frames;
  if (std::holds_alternative<uart::Frame>(msg.decoded)) {
    // 復号できたフレームなら何でも生存確認を更新する。最初の受信までは数えない（open-questions #3）
    timers_.schedule(clock_.steady_now() + constants::kUartLiveness, kLivenessTimer);
  }
  auto result = bridge_.receive(msg.decoded, msg.received_at);
  if (result.envelope) {
    if (result.envelope->schema == "iot.v1.state") {
      uart_state_online_ = result.envelope->payload["status"] == "online";
    }
    publish(*result.envelope, out);
  }
  if (result.request_state) {
    queue_get_status();
  }
  send_pending_cmds(out);
}

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
        publish_uart_offline(out);
        break;
      case TimerKind::kUartCmdSlot:
        send_pending_cmds(out);
        break;
    }
  }
  return out;
}

std::optional<SteadyTime> Core::next_deadline() {
  return timers_.next_deadline();
}

void Core::publish_uart_offline(Outputs& out) {
  if (!uart_state_online_) {
    return;
  }
  const auto offline = bridge_.offline_state(clock_.system_now());
  if (!offline) {
    return;
  }
  uart_state_online_ = false;
  ++stats_.uart_offline_published;
  publish(*offline, out);
}

void Core::publish(const Envelope& env, Outputs& out) const {
  const auto kind = env.kind();
  const auto info = require_kind(kind);
  out.mqtt.push_back(MqttTx{topic_for(config_.site, env.device_id, kind), env.to_json().dump(),
                            info.qos, info.retain, env.device_id, kind});
}

Envelope Core::gw_state_envelope(const std::string& status, std::uint32_t seq) {
  Json payload = Json::object();
  payload["status"] = status;
  payload["mode"] = kGwInitialMode;
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
  Envelope env;
  env.schema = std::string{require_kind(kKindState).schema};
  env.msg_id = ids_.next_ulid();
  env.device_id = config_.device_id;
  env.seq = seq;
  env.boot_id = config_.boot_id;
  env.ts = clock_.system_now();
  env.ts_source = "device";
  env.payload = std::move(payload);
  return env;
}

void Core::publish_gw_state(const std::string& status, Outputs& out) {
  publish(gw_state_envelope(status, gw_seq_.next(std::string{kKindState})), out);
}

MqttTx Core::gw_offline_state() {
  Outputs out;
  publish(gw_state_envelope("offline", gw_seq_.peek(std::string{kKindState})), out);
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
