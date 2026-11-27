// Core のうち、履歴・起動時の復元・gwctl の管理要求にかかわる部分（Phase 5）。
//
// - 履歴の行は Outputs::history に積むだけ（書くのは history スレッド。core は DB を読まない：D5）
// - restore：直近1時間の要求（#42）、台帳（#43）、EMERGENCY の引き継ぎ（#44）
// - 管理要求：devices・rules・stats・approve（approved_by_gwctl）・cmd（#46）・diag（#47）。
//   reload は admin スレッドが app の再読込（SIGHUP と同じ）を呼ぶので core には来ない
#include <algorithm>
#include <set>

#include "core/Core.h"
#include "core/common/Constants.h"
#include "core/common/Optional.h"

namespace gw::core {

namespace {

constexpr std::array<std::string_view, 5> kOps{"set_actuator", "release", "set_mode", "set_param",
                                               "get_status"};

void channel_leaves(const rules::Condition& c, std::set<rules::ChannelKey>& out) {
  if (const auto* leaf = std::get_if<rules::ChannelLeaf>(&c.leaf)) {
    out.insert({leaf->device, leaf->channel});
  }
  for (const auto& child : c.children) {
    channel_leaves(child, out);
  }
}

Json error(const std::string& message) {
  return Json{{"ok", false}, {"error", message}};
}

Json ok(Json result) {
  return Json{{"ok", true}, {"result", std::move(result)}};
}

}  // namespace

std::int64_t Core::now_ms() const {
  return clock_.system_now().time_since_epoch().count();
}

void Core::record(history::Row row, Outputs& out) const {
  out.history.push_back(history::record(std::move(row)));
}

void Core::record_event(const Json& message, Outputs& out) const {
  const auto& p = message["payload"];
  const auto ts = parse_ts(message.value("ts", ""));
  record(history::EventRow{message.value("msg_id", ""), message.value("device_id", ""),
                           p.value("code", ""), p.value("severity", ""), p.dump(),
                           ts ? ts->time_since_epoch().count() : now_ms()},
         out);
}

void Core::record_commands(const command::CommandOutputs& cmd_out, Outputs& out) const {
  const auto ms = now_ms();
  for (const auto& e : cmd_out.events) {
    const auto& p = e.payload;
    record(history::CommandRow{e.cmd_id, e.target, p.value("op", ""),
                               p.contains("args") ? p["args"].dump() : "{}",
                               p.contains("origin") ? p["origin"].dump() : "{}",
                               p.value("priority", ""), e.to, e.retries, ms, ms},
           out);
    record(
        history::CommandTransitionRow{e.cmd_id, e.from.empty() ? "-" : e.from, e.to, e.detail, ms},
        out);
  }
}

void Core::record_request(const std::string& request_id, Outputs& out) const {
  const auto* r = requests_.find(request_id);
  if (r == nullptr) {
    return;
  }
  record(history::RequestRow{r->request_id, r->requester, r->kind, r->body.dump(), r->status,
                             r->reason.value_or(""), r->cmd_id.value_or(""), now_ms()},
         out);
}

Json Core::rule_trigger(std::size_t i, std::string_view what) const {
  std::set<rules::ChannelKey> keys;
  channel_leaves(rules_.rules().at(i).when, keys);
  Json values = Json::object();
  for (const auto& [device, channel] : keys) {
    const auto* info = registry_.find(device);
    std::string key = device;
    key += "/";
    key += channel;
    const double* value = nullptr;
    if (info != nullptr) {
      if (const auto it = info->channels.find(channel); it != info->channels.end()) {
        value = opt_ptr(it->second.value);
      }
    }
    values[key] = value != nullptr ? Json(*value) : Json(nullptr);
  }
  return Json{{"event", what}, {"mode", mode_.mode()}, {"values", std::move(values)}};
}

// ------------------------------------------------------------ 起動時の復元

void Core::restore(const CoreRestore& r) {
  requests_.restore(r.requests, clock_.steady_now());
  for (const auto& d : r.devices) {
    const auto channels = Json::parse(d.channels.empty() ? "[]" : d.channels, nullptr, false);
    const auto actuators = Json::parse(d.actuators.empty() ? "[]" : d.actuators, nullptr, false);
    registry_.restore(d.device_id, d.transport, channels.is_discarded() ? Json::array() : channels,
                      actuators.is_discarded() ? Json::array() : actuators);
  }
  if (r.last_mode && *r.last_mode == mode::kEmergency) {
    // EMERGENCY のまま再起動した：EMERGENCY で起動し、承認を待つ（安全側。open-questions #44）
    std::optional<std::string> rule;
    if (rules_.find(r.last_mode_reason)) {
      rule = r.last_mode_reason;
    }
    mode_.restore_emergency(rule);
  }
}

// ------------------------------------------------------------ gwctl

Json Core::admin_devices() const {
  Json list = Json::array();
  for (const auto& [id, info] : registry_.devices()) {
    const auto status = registry_.effective_status(id);
    Json channels = Json::array();
    for (const auto& [name, ch] : info.channels) {
      channels.push_back(
          Json{{"channel", name},
               {"unit", ch.unit},
               {"value", opt_ptr(ch.value) != nullptr ? Json(*opt_ptr(ch.value)) : Json(nullptr)},
               {"quality", ch.quality},
               {"stale", ch.stale}});
    }
    list.push_back(Json{
        {"device_id", id},
        {"transport", info.transport},
        // まだ観測していないデバイスは unknown と表示する（#43）
        {"status", opt_ptr(status) != nullptr ? std::string{device::to_string(*opt_ptr(status))}
                                              : std::string{"unknown"}},
        {"static", info.is_static},
        {"boot_id",
         opt_ptr(info.boot_id) != nullptr ? Json(*opt_ptr(info.boot_id)) : Json(nullptr)},
        {"bridged_by",
         opt_ptr(info.bridged_by) != nullptr ? Json(*opt_ptr(info.bridged_by)) : Json(nullptr)},
        {"channels", std::move(channels)}});
  }
  return list;
}

Json Core::admin_rules() const {
  Json list = Json::array();
  for (std::size_t i = 0; i < rules_.rules().size(); ++i) {
    const auto& rule = rules_.rules()[i];
    list.push_back(Json{{"id", rule.id},
                        {"priority", std::string{command::to_string(rule.priority)}},
                        {"state", std::string{rules::to_string(rules_.state(i))}},
                        {"unknown", rules_.unknown(i)},
                        {"silenced", rules_.silenced(i)}});
  }
  return Json{{"mode", mode_.mode()}, {"rules", std::move(list)}};
}

Json Core::admin_stats() const {
  Json counts = Json::object();
  const auto quality = quality_counts();  // 一時オブジェクトの中を範囲 for でたどらない
  for (const auto& r : quality["readings"]) {
    counts[r["channel"].get<std::string>()] = r["value"];
  }
  const auto& c = commands_.stats();
  return Json{{"mode", mode_.mode()},
              {"quality", std::move(counts)},
              {"commands", Json{{"submitted", c.submitted},
                                {"done", c.done},
                                {"rejected", c.rejected},
                                {"failed", c.failed},
                                {"timed_out", c.timed_out},
                                {"in_flight_uart", commands_.uart_in_flight()},
                                {"queued_uart", commands_.uart_queued()}}},
              {"requests", Json{{"received", requests_.stats().received},
                                {"duplicates", requests_.stats().duplicates},
                                {"approvals", requests_.stats().approvals}}},
              {"rules", Json{{"fired", stats_.rules_fired}, {"cleared", stats_.rules_cleared}}}};
}

void Core::on_admin(const AdminRequest& msg, Outputs& out) {
  const auto& args = msg.args;
  if (msg.cmd == "devices") {
    out.admin.push_back(AdminReply{msg.id, ok(admin_devices())});
  } else if (msg.cmd == "rules") {
    out.admin.push_back(AdminReply{msg.id, ok(admin_rules())});
  } else if (msg.cmd == "stats") {
    out.admin.push_back(AdminReply{msg.id, ok(admin_stats())});
  } else if (msg.cmd == "approve") {
    // EMERGENCY の解除の承認（reason approved_by_gwctl。v0.2.3 #48）
    const auto id = approve_emergency_release(mode::kApprovedByGwctl);
    command::CommandOutputs cmd_out;
    std::vector<command::RequestAck> acks;
    flush(cmd_out, acks, out);  // SYSTEM_MODE_CHANGED と state
    Json result{{"mode", mode_.mode()}};
    if (id) {
      result["status"] = "done";
      result["cmd_id"] = *id;
    } else {
      result["status"] = "rejected";
      result["reason"] = "unsafe";  // 条件が回復していない
    }
    out.admin.push_back(AdminReply{msg.id, ok(std::move(result))});
  } else if (msg.cmd == "cmd") {
    // 手動の指令（優先度 manual、origin gwctl）。調停と指令管理は request と同じ経路（#46）
    const auto op = args.value("op", "");
    if (args.value("target", "").empty() || std::find(kOps.begin(), kOps.end(), op) == kOps.end()) {
      out.admin.push_back(AdminReply{msg.id, error("target と op（共通仕様の op）が要る")});
      return;
    }
    Json request{{"request_id", ids_.next_ulid()},
                 {"kind", "manual"},
                 {"target", args["target"]},
                 {"op", op}};
    if (args.contains("args") && args["args"].is_object() && !args["args"].empty()) {
      request["args"] = args["args"];
    }
    if (args.contains("lease_s")) {
      request["lease_s"] = args["lease_s"];
    }
    const auto request_id = request["request_id"].get<std::string>();
    admin_requests_[request_id] = msg.id;
    command::CommandOutputs cmd_out;
    std::vector<command::RequestAck> acks;
    requests_.handle(request, std::string{kGwctlRequester}, clock_.steady_now(), acks, cmd_out,
                     true);
    flush(cmd_out, acks, out);
  } else if (msg.cmd == "diag") {
    // GET_DIAG を内部の指令として送る。DIAG は履歴に入り、admin スレッドが DB から読む（#47）
    const auto device_id = args.value("device", "");
    if (device_id != config_.uart_profile.device_id) {
      out.admin.push_back(AdminReply{msg.id, error("UART のデバイスではない: " + device_id)});
      return;
    }
    const auto requested_ms = now_ms();
    command::CommandOutputs cmd_out;
    std::vector<command::RequestAck> acks;
    const auto cmd_id = commands_.submit_internal(
        command::CommandManager::Submit{device_id, Json{{"op", "get_diag"}, {"priority", "manual"}},
                                        "", true},
        constants::kUartOpGetDiag, cmd_out);
    const bool failed = commands_.find(cmd_id) == nullptr;
    flush(cmd_out, acks, out);
    if (failed) {
      out.admin.push_back(AdminReply{msg.id, error(device_id + " は offline")});
      return;
    }
    out.admin.push_back(AdminReply{msg.id, ok(Json{{"status", "sent"},
                                                   {"device", device_id},
                                                   {"cmd_id", cmd_id},
                                                   {"requested_ms", requested_ms}})});
  } else {
    out.admin.push_back(AdminReply{msg.id, error("知らないコマンド: " + msg.cmd)});
  }
}

}  // namespace gw::core
