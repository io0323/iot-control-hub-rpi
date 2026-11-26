#include "core/command/RequestHandler.h"

#include <algorithm>
#include <utility>

#include "core/common/Constants.h"

namespace gw::core::command {

namespace {

constexpr std::chrono::seconds kRequestTtl{constants::kRequestDedupTtlS};
constexpr std::string_view kSetActuator = "set_actuator";
constexpr std::string_view kRelease = "release";

// request/ack の reason（第3章）。作品03 の内部の理由を共通仕様の値にする（open-questions #22）
std::string request_reason(const CommandRecord& cmd) {
  if (cmd.state == CmdState::kTimedOut || cmd.reason == kReasonOffline) {
    return "target_offline";
  }
  return cmd.reason.empty() ? "internal" : cmd.reason;
}

}  // namespace

bool RequestRules::permits(const std::string& target, const std::string& op,
                           const std::string& actuator) const {
  return std::ranges::any_of(allow, [&](const RequestRule& rule) {
    return rule.target == target && rule.op == op &&
           (!rule.actuators.has_value() || rule.actuators.value().contains(actuator));
  });
}

RequestHandler::RequestHandler(RequestRules rules, Arbiter& arbiter, CommandManager& commands,
                               DeviceFn known, DeviceFn online, OwnerChangeFn on_owner_change)
    : rules_(std::move(rules)),
      arbiter_(arbiter),
      commands_(commands),
      known_(std::move(known)),
      online_(std::move(online)),
      on_owner_change_(std::move(on_owner_change)) {
}

void RequestHandler::respond(const RequestRecord& r, std::vector<RequestAck>& acks) {
  Json payload = Json::object();
  payload["request_id"] = r.request_id;
  payload["status"] = r.status;
  if (r.cmd_id) {
    payload["cmd_id"] = *r.cmd_id;
  }
  const bool has_reason = r.status == "rejected" || r.status == "failed";
  payload["reason"] = has_reason && r.reason ? Json(*r.reason) : Json(nullptr);
  acks.push_back(RequestAck{r.requester, std::move(payload)});
}

void RequestHandler::reject(RequestRecord& r, std::string reason, std::vector<RequestAck>& acks) {
  ++stats_.rejected;
  r.status = "rejected";
  r.reason = std::move(reason);
  respond(r, acks);
}

void RequestHandler::approve(RequestRecord& r, const Json& request,
                             std::vector<RequestAck>& acks) {
  const Json args =
      request.contains("args") && request["args"].is_object() ? request["args"] : Json::object();
  // kind manual の set_mode NORMAL だけが承認。ほかは not_permitted（#46、open-questions #36）
  if (request.value("kind", "") != "manual" || r.op != "set_mode" ||
      args.value("mode", "") != "NORMAL" || !hooks_.approve) {
    reject(r, "not_permitted", acks);
    return;
  }
  auto id = hooks_.approve();
  if (!id) {
    reject(r, "unsafe", acks);  // 条件が回復していない
    return;
  }
  ++stats_.accepted;
  ++stats_.approvals;
  r.status = "accepted";
  r.cmd_id = std::move(*id);
  respond(r, acks);
  ++stats_.done;
  r.status = "done";
  respond(r, acks);
}

void RequestHandler::handle(const Json& request, const std::string& requester, SteadyTime now,
                            std::vector<RequestAck>& acks, CommandOutputs& cmd_out) {
  purge(now);
  ++stats_.received;
  const auto request_id = request.value("request_id", "");
  if (const auto it = records_.find(request_id); it != records_.end()) {
    ++stats_.duplicates;
    respond(it->second, acks);  // 重複には新しい指令を作らず、現在の状態を返す
    return;
  }
  auto& r = records_[request_id];
  r.request_id = request_id;
  r.requester = requester;
  r.received_at = now;
  r.op = request.value("op", "");
  const auto target = request.value("target", "");
  const Json args =
      request.contains("args") && request["args"].is_object() ? request["args"] : Json::object();
  const auto actuator =
      args.contains("id") && args["id"].is_string() ? args["id"].get<std::string>() : "";
  const auto priority =
      request.value("kind", "") == "schedule" ? Priority::kSchedule : Priority::kManual;

  if (!hooks_.self_id.empty() && target == hooks_.self_id) {
    approve(r, request, acks);  // gw-01 宛て：EMERGENCY の解除の承認（v0.2.3 #46）
    return;
  }
  if (!known_(target) || !rules_.permits(target, r.op, actuator)) {
    reject(r, "not_permitted", acks);
    return;
  }
  if (priority == Priority::kSchedule && hooks_.emergency && hooks_.emergency()) {
    reject(r, "emergency_mode", acks);  // EMERGENCY ではスケジュールを止める（FR-12）
    return;
  }
  if (!online_(target)) {
    reject(r, "target_offline", acks);  // 指令を作らない（#22）
    return;
  }
  const auto owner = "request:" + request_id;
  const Json origin{{"kind", "request"}, {"ref", request_id}};
  Json payload = Json::object();
  payload["op"] = r.op;
  if (!args.empty()) {
    payload["args"] = args;
  }
  payload["priority"] = std::string{to_string(priority)};
  std::optional<std::string> cmd_id;
  if (r.op == kSetActuator) {
    const auto lease_s = request.value("lease_s", constants::kLeaseDefaultS);
    if (lease_s < constants::kLeaseMinS || lease_s > constants::kLeaseMaxS) {
      reject(r, "invalid_arg", acks);
      return;
    }
    payload["lease_s"] = lease_s;
    const ActuatorKey key{target, actuator};
    const auto result = arbiter_.claim(
        key, Claim{owner, priority, args, origin, now + std::chrono::seconds{lease_s}, 0}, now);
    if (!result.accepted) {
      reject(r, "lower_priority", acks);
      return;
    }
    r.key = key;
    r.owner = owner;
  } else if (r.op == kRelease) {
    const ActuatorKey key{target, actuator};
    if (const auto* current = arbiter_.owner(key);
        current != nullptr && !at_least(priority, current->priority)) {
      reject(r, "lower_priority", acks);
      return;
    }
    const auto* current = arbiter_.owner(key);
    const std::string previous = current != nullptr ? current->owner : "";
    auto change = arbiter_.release_for(key, priority, now);
    if (change && hooks_.released) {
      hooks_.released(previous);  // ルールの所有を外したら、そのルールは再発火まで黙る（#26）
    }
    if (change && change->new_owner) {
      // 待ちが引き継ぐ（#24）。その指令の結果をこの request の結果にする。
      // 待ちが空・所有者がいなければ、下で release の指令を送る
      cmd_id = on_owner_change_(*change, cmd_out);
    }
  }
  payload["origin"] = origin;
  if (!cmd_id) {
    cmd_id =
        commands_.submit(CommandManager::Submit{target, std::move(payload), owner, true}, cmd_out);
  }
  ++stats_.accepted;
  r.status = "accepted";
  r.cmd_id = *cmd_id;
  by_cmd_[*cmd_id] = request_id;
  respond(r, acks);
}

void RequestHandler::on_command_finished(const CommandRecord& cmd, SteadyTime now,
                                         std::vector<RequestAck>& acks, CommandOutputs& cmd_out) {
  const auto link = by_cmd_.find(cmd.cmd_id);
  if (link == by_cmd_.end()) {
    return;
  }
  const auto request_id = link->second;
  by_cmd_.erase(link);
  const auto it = records_.find(request_id);
  if (it == records_.end() || it->second.status != "accepted") {
    return;
  }
  auto& r = it->second;
  if (cmd.state == CmdState::kDone) {
    ++stats_.done;
    r.status = "done";
    r.reason.reset();
  } else if (cmd.state == CmdState::kRejected && cmd.reason == kReasonExpired) {
    ++stats_.rejected;
    r.status = "rejected";  // expires_at を過ぎて送らなかった（#40）
    r.reason = std::string{kReasonExpired};
  } else {
    ++stats_.failed;
    r.status = "failed";
    r.reason = request_reason(cmd);
  }
  if (cmd.state != CmdState::kDone && r.key) {
    // 実行されなかった主張は外す。奪われていた所有者がいれば引き継ぐ（release は送らない）
    if (auto change = arbiter_.release(*r.key, r.owner, now)) {
      on_owner_change_(*change, cmd_out);
    }
  }
  respond(r, acks);
}

void RequestHandler::purge(SteadyTime now) {
  for (auto it = records_.begin(); it != records_.end();) {
    if (now - it->second.received_at >= kRequestTtl) {
      if (const auto& cmd_id = it->second.cmd_id; cmd_id.has_value()) {
        by_cmd_.erase(cmd_id.value());
      }
      it = records_.erase(it);
    } else {
      ++it;
    }
  }
}

const RequestRecord* RequestHandler::find(const std::string& request_id) const {
  const auto it = records_.find(request_id);
  return it == records_.end() ? nullptr : &it->second;
}

std::optional<SteadyTime> RequestHandler::next_purge() const {
  std::optional<SteadyTime> next;
  for (const auto& [id, r] : records_) {
    const auto at = r.received_at + kRequestTtl;
    if (!next || at < *next) {
      next = at;
    }
  }
  return next;
}

}  // namespace gw::core::command
