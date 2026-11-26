#include "core/command/CommandManager.h"

#include <algorithm>
#include <utility>

#include "core/common/Constants.h"
#include "core/common/Optional.h"

namespace gw::core::command {

namespace {

constexpr std::chrono::seconds kUartAckTimeout{constants::kUartAckTimeoutS};
constexpr std::chrono::seconds kMqttAckTimeout{constants::kCmdAckTimeoutS};

}  // namespace

std::string_view to_string(CmdState s) {
  switch (s) {
    case CmdState::kIssued:
      return "ISSUED";
    case CmdState::kSent:
      return "SENT";
    case CmdState::kAccepted:
      return "ACCEPTED";
    case CmdState::kDone:
      return "DONE";
    case CmdState::kRejected:
      return "REJECTED";
    case CmdState::kFailed:
      return "FAILED";
    case CmdState::kTimedOut:
      return "TIMED_OUT";
  }
  return "UNKNOWN";
}

bool is_terminal(CmdState s) {
  return s == CmdState::kDone || s == CmdState::kRejected || s == CmdState::kFailed ||
         s == CmdState::kTimedOut;
}

CommandManager::CommandManager(uart::UartBridge& bridge, IClock& clock, IIdGenerator& ids,
                               std::string uart_device_id, OnlineFn online)
    : bridge_(bridge),
      clock_(clock),
      ids_(ids),
      uart_device_id_(std::move(uart_device_id)),
      online_(std::move(online)) {
}

std::string CommandManager::actuator_of(const Json& payload) {
  if (payload.contains("args") && payload["args"].is_object() && payload["args"].contains("id") &&
      payload["args"]["id"].is_string()) {
    return payload["args"]["id"].get<std::string>();
  }
  return {};
}

std::string CommandManager::submit(Submit s, CommandOutputs& out) {
  ++stats_.submitted;
  const auto now = clock_.steady_now();
  CommandRecord r;
  r.cmd_id = ids_.next_ulid();
  r.target = std::move(s.target);
  r.owner = std::move(s.owner);
  r.route = r.target == uart_device_id_ ? Route::kUart : Route::kMqtt;
  // cmd_id を先頭に置く（共通仕様 第3章の例と同じ並び）
  Json payload = Json::object();
  payload["cmd_id"] = r.cmd_id;
  for (const auto& [key, value] : s.payload.items()) {
    if (key != "cmd_id") {
      payload[key] = value;
    }
  }
  if (!payload.contains("priority")) {
    payload["priority"] = "auto";
  }
  r.payload = std::move(payload);
  std::string cmd_id = r.cmd_id;
  const bool check_online = s.check_online;
  active_.emplace(cmd_id, std::move(r));
  auto& record = active_.at(cmd_id);
  if (check_online && !online_(record.target)) {
    ++stats_.offline;
    finish(cmd_id, CmdState::kFailed, std::string{kReasonOffline}, out);
    return cmd_id;
  }
  if (record.route == Route::kMqtt) {
    if (expired(record)) {
      ++stats_.expired;
      finish(cmd_id, CmdState::kRejected, std::string{kReasonExpired}, out);
      return cmd_id;
    }
    transmit_mqtt(record, now, out);
    return cmd_id;
  }
  replace_queued(record, out);
  uart_queue_.push_back(cmd_id);
  pump_uart(now, out);
  return cmd_id;
}

void CommandManager::replace_queued(const CommandRecord& incoming, CommandOutputs& out) {
  if (incoming.owner.empty()) {
    return;
  }
  const auto actuator = actuator_of(incoming.payload);
  const auto op = incoming.payload.value("op", "");
  for (auto it = uart_queue_.begin(); it != uart_queue_.end();) {
    const auto found = active_.find(*it);
    if (found != active_.end() && found->second.owner == incoming.owner &&
        found->second.target == incoming.target && found->second.payload.value("op", "") == op &&
        actuator_of(found->second.payload) == actuator) {
      ++stats_.replaced;
      out.replaced.push_back(std::move(found->second));
      active_.erase(found);
      it = uart_queue_.erase(it);
    } else {
      ++it;
    }
  }
}

void CommandManager::finish(const std::string& cmd_id, CmdState state, std::string reason,
                            CommandOutputs& out) {
  const auto it = active_.find(cmd_id);
  if (it == active_.end()) {
    return;
  }
  auto& r = it->second;
  r.state = state;
  r.reason = std::move(reason);
  switch (state) {
    case CmdState::kDone:
      ++stats_.done;
      break;
    case CmdState::kRejected:
      ++stats_.rejected;
      break;
    case CmdState::kFailed:
      ++stats_.failed;
      break;
    case CmdState::kTimedOut:
      ++stats_.timed_out;
      break;
    default:
      break;
  }
  if (r.cmd_ref) {
    const auto in_flight = uart_in_flight_.find(*r.cmd_ref);
    if (in_flight != uart_in_flight_.end() && in_flight->second == cmd_id) {
      uart_in_flight_.erase(in_flight);
    }
  }
  std::erase(uart_queue_, cmd_id);
  out.finished.push_back(std::move(r));
  active_.erase(it);
}

bool CommandManager::expired(const CommandRecord& r) const {
  if (!r.payload.contains("expires_at") || !r.payload["expires_at"].is_string()) {
    return false;
  }
  const auto at = parse_ts(r.payload["expires_at"].get<std::string>());
  return at && clock_.system_now() >= *at;
}

bool CommandManager::slot_free(SteadyTime now) const {
  return !last_uart_sent_ || now >= *last_uart_sent_ + constants::kUartCmdInterval;
}

void CommandManager::transmit_mqtt(CommandRecord& r, SteadyTime now, CommandOutputs& out) {
  out.mqtt.push_back(CommandOutputs::Publish{r.target, r.payload});
  ++stats_.mqtt_sent;
  if (r.attempts > 0) {
    ++stats_.mqtt_retries;
  }
  ++r.attempts;
  r.sent_at = now;
  r.state = CmdState::kSent;
}

bool CommandManager::transmit_uart(CommandRecord& r, SteadyTime now, CommandOutputs& out) {
  if (r.attempts == 0 && expired(r)) {
    // 期限の検査は最初の送信の前だけ（#43）。UART に送らず、cmd/ack も作らない（#40）
    ++stats_.expired;
    finish(r.cmd_id, CmdState::kRejected, std::string{kReasonExpired}, out);
    return false;
  }
  if (!r.cmd_ref) {
    r.cmd_ref = next_cmd_ref_;
    next_cmd_ref_ = static_cast<std::uint16_t>((next_cmd_ref_ + 1U) % constants::kCmdRefModulo);
  }
  auto frame = bridge_.command_to_frame(r.payload, *r.cmd_ref);
  if (auto* rejected = std::get_if<uart::CommandRejected>(&frame)) {
    finish(r.cmd_id, CmdState::kRejected, rejected->reason, out);
    return false;
  }
  out.uart.push_back(std::get<uart::Cmd>(frame));
  ++stats_.uart_sent;
  if (r.attempts > 0) {
    ++stats_.uart_retries;
  }
  ++r.attempts;
  r.sent_at = now;
  r.state = CmdState::kSent;
  last_uart_sent_ = now;
  return true;
}

void CommandManager::pump_uart(SteadyTime now, CommandOutputs& out) {
  // uart-out に入らなかった CMD を先に
  while (!uart_requeued_.empty() && slot_free(now)) {
    out.uart.push_back(uart_requeued_.front());
    const auto ref = uart_requeued_.front().cmd_ref;
    uart_requeued_.pop_front();
    last_uart_sent_ = now;
    if (const auto it = uart_in_flight_.find(ref); it != uart_in_flight_.end()) {
      active_.at(it->second).sent_at = now;
    }
  }
  // 再送（ACK が2秒来ない指令）を新しい指令より先に（第4章、v0.2.1）
  std::vector<std::pair<std::uint16_t, std::string>> in_flight(uart_in_flight_.begin(),
                                                               uart_in_flight_.end());
  std::sort(in_flight.begin(), in_flight.end(), [&](const auto& a, const auto& b) {
    return active_.at(a.second).sent_at < active_.at(b.second).sent_at;
  });
  for (const auto& [ref, cmd_id] : in_flight) {
    auto& r = active_.at(cmd_id);
    if (!r.sent_at || now - *r.sent_at < kUartAckTimeout) {
      continue;
    }
    if (r.attempts > constants::kUartCmdMaxRetries) {
      finish(cmd_id, CmdState::kTimedOut, std::string{kReasonTimeout}, out);
      continue;
    }
    if (slot_free(now)) {
      transmit_uart(r, now, out);
    }
  }
  // 新しい指令（毎秒1件・未完了4件まで）
  while (!uart_queue_.empty() &&
         uart_in_flight_.size() < static_cast<std::size_t>(constants::kUartCmdMaxInFlight) &&
         slot_free(now)) {
    const auto cmd_id = uart_queue_.front();
    uart_queue_.pop_front();
    auto& r = active_.at(cmd_id);
    if (transmit_uart(r, now, out) && r.cmd_ref.has_value()) {
      uart_in_flight_[r.cmd_ref.value()] = cmd_id;
    }
  }
}

void CommandManager::on_ack(const Json& ack, CommandOutputs& out) {
  const auto cmd_id = ack.value("cmd_id", "");
  const auto it = active_.find(cmd_id);
  if (it == active_.end()) {
    ++stats_.unknown_ack;  // 終わった指令への遅れた ACK・重複した ACK
    return;
  }
  auto& r = it->second;
  if (r.cmd_ref) {
    // どの ACK でも未完了から外す（gw_01_lite._on_ack と同じ）
    const auto in_flight = uart_in_flight_.find(*r.cmd_ref);
    if (in_flight != uart_in_flight_.end() && in_flight->second == cmd_id) {
      uart_in_flight_.erase(in_flight);
    }
  }
  r.limited = ack.value("limited", false);
  const auto status = ack.value("status", "");
  const auto reason = ack.contains("reason") && ack["reason"].is_string()
                          ? ack["reason"].get<std::string>()
                          : std::string{};
  if (status == "accepted") {
    if (r.state == CmdState::kAccepted) {
      ++stats_.duplicate_ack;
    }
    r.state = CmdState::kAccepted;
  } else if (status == "done") {
    finish(cmd_id, CmdState::kDone, {}, out);
  } else if (status == "rejected") {
    finish(cmd_id, CmdState::kRejected, reason.empty() ? "internal" : reason, out);
  } else if (status == "failed") {
    finish(cmd_id, CmdState::kFailed, reason.empty() ? "internal" : reason, out);
  }
  pump_uart(clock_.steady_now(), out);  // 未完了の枠が空いた
}

void CommandManager::on_timer(SteadyTime now, CommandOutputs& out) {
  renew_due(now, out);
  // MQTT の再送とタイムアウト
  std::vector<std::string> due;
  for (const auto& [cmd_id, r] : active_) {
    const auto* sent = opt_ptr(r.sent_at);
    if (r.route == Route::kMqtt && r.state == CmdState::kSent && sent != nullptr &&
        now - *sent >= kMqttAckTimeout) {
      due.push_back(cmd_id);
    }
  }
  for (const auto& cmd_id : due) {
    auto& r = active_.at(cmd_id);
    if (r.attempts > constants::kCmdMaxRetries) {
      finish(cmd_id, CmdState::kTimedOut, std::string{kReasonTimeout}, out);
    } else {
      transmit_mqtt(r, now, out);
    }
  }
  pump_uart(now, out);
}

void CommandManager::requeue_uart_frame(const uart::Cmd& frame) {
  ++stats_.uart_requeued;
  uart_requeued_.push_front(frame);
}

SteadyTime::duration CommandManager::renew_interval(const Json& payload) {
  const auto lease_s = payload.value("lease_s", constants::kLeaseDefaultS);
  return std::chrono::duration_cast<SteadyTime::duration>(
      std::chrono::milliseconds{lease_s * 1000} / constants::kLeaseRenewDivisor);
}

std::string CommandManager::keep(const std::string& owner, const std::string& target, Json payload,
                                 CommandOutputs& out) {
  const auto actuator = actuator_of(payload);
  stop_keeping(owner, target, actuator);
  if (!payload.contains("lease_s")) {
    payload["lease_s"] = constants::kLeaseDefaultS;
  }
  const auto next_at = clock_.steady_now() + renew_interval(payload);
  keeps_.push_back(Keep{owner, target, actuator, payload, next_at});
  return submit(Submit{target, std::move(payload), owner, true}, out);
}

void CommandManager::stop_keeping(const std::string& owner, const std::string& target,
                                  const std::string& actuator) {
  std::erase_if(keeps_, [&](const Keep& k) {
    return k.owner == owner && k.target == target && k.actuator == actuator;
  });
}

void CommandManager::stop_keeping(const std::string& owner) {
  std::erase_if(keeps_, [&](const Keep& k) { return k.owner == owner; });
}

bool CommandManager::keeping(const std::string& owner) const {
  return std::any_of(keeps_.begin(), keeps_.end(),
                     [&](const Keep& k) { return k.owner == owner; });
}

void CommandManager::renew_due(SteadyTime now, CommandOutputs& out) {
  std::vector<Keep> due;
  for (auto& k : keeps_) {
    if (now >= k.next_at) {
      k.next_at += renew_interval(k.payload);
      due.push_back(k);
    }
  }
  for (auto& k : due) {
    if (renew_guard_ && !renew_guard_(k.owner, k.target, k.payload)) {
      ++stats_.renewals_skipped;
      continue;
    }
    ++stats_.renewals;
    // 新しい cmd_id で送り直す。送信待ちに古い更新が残っていれば置き換える（#21）
    submit(Submit{k.target, std::move(k.payload), k.owner, true}, out);
  }
}

std::optional<SteadyTime> CommandManager::next_deadline() const {
  std::optional<SteadyTime> next;
  const auto consider = [&](SteadyTime at) {
    if (!next || at < *next) {
      next = at;
    }
  };
  const auto slot =
      last_uart_sent_ ? *last_uart_sent_ + constants::kUartCmdInterval : clock_.steady_now();
  if (!uart_queue_.empty() || !uart_requeued_.empty()) {
    consider(slot);
  }
  for (const auto& [ref, cmd_id] : uart_in_flight_) {
    const auto& r = active_.at(cmd_id);
    if (r.sent_at) {
      // 再送の時期が来ても送信枠が空くまでは待つ（同じ時刻で回り続けないように）
      consider(std::max(*r.sent_at + kUartAckTimeout, slot));
    }
  }
  for (const auto& [cmd_id, r] : active_) {
    const auto* sent = opt_ptr(r.sent_at);
    if (r.route == Route::kMqtt && r.state == CmdState::kSent && sent != nullptr) {
      consider(*sent + kMqttAckTimeout);
    }
  }
  for (const auto& k : keeps_) {
    consider(k.next_at);
  }
  return next;
}

const CommandRecord* CommandManager::find(const std::string& cmd_id) const {
  const auto it = active_.find(cmd_id);
  return it == active_.end() ? nullptr : &it->second;
}

}  // namespace gw::core::command
