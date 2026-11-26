// 要求の受付（基本設計 第6章、FR-10。共通仕様 第3章 request・request/ack、第4章）。
// 対応する Python：iotspec.sim.devices.gw_01_lite（_on_request・_respond・_finish_request）
//
// - requests.yaml の許可リストで判定する（open-questions #20）。優先度は request の kind
//   （manual・schedule）で決まる
// - set_actuator・release は Arbiter で調停する。ほかの op は調停しない
// - request_id を1時間記憶し、重複には新しい指令を作らず現在の状態を返す（D5。再起動をまたぐ分は Phase 5）
// - request/ack は accepted（cmd_id）→ done／failed、または rejected（reason）。
//   結果と reason の対応は open-questions #22
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/command/Arbiter.h"
#include "core/command/CommandManager.h"
#include "core/common/Clock.h"
#include "core/common/Envelope.h"

namespace gw::core::command {

// requests.yaml の1項目（open-questions #20）
struct RequestRule {
  std::string target;
  std::string op;
  std::optional<std::set<std::string>> actuators;  // set_actuator・release で許す id。省略で全部
};

struct RequestRules {
  std::vector<RequestRule> allow;
  [[nodiscard]] bool permits(const std::string& target, const std::string& op,
                             const std::string& actuator) const;
};

struct RequestStats {
  std::uint64_t received = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t done = 0;
  std::uint64_t failed = 0;
  std::uint64_t approvals = 0;  // 承認で NORMAL に戻した（#46）
};

// 1件の request の現在の状態
struct RequestRecord {
  std::string request_id;
  std::string requester;  // request を送ったデバイス（request/ack のトピック）
  std::string status;     // accepted・rejected・done・failed
  std::optional<std::string> cmd_id;
  std::optional<std::string> reason;
  std::string op;
  std::optional<ActuatorKey> key;  // set_actuator の対象
  std::string owner;               // Arbiter の主張の主（request:<request_id>）
  SteadyTime received_at{};
};

// 発行する request/ack（requester はトピックの device_id）
struct RequestAck {
  std::string requester;
  Json payload;
};

class RequestHandler {
 public:
  // 宛先が登録済みか・online か（Core の DeviceRegistry・LivenessMonitor で判定する）
  using DeviceFn = std::function<bool(const std::string&)>;
  // 所有者が変わったときの処理（Core：新しい所有者がいれば引き継ぎの指令を送る）。
  // 送った指令の cmd_id を返す
  using OwnerChangeFn =
      std::function<std::optional<std::string>(const OwnerChange&, CommandOutputs&)>;

  RequestHandler(RequestRules rules, Arbiter& arbiter, CommandManager& commands, DeviceFn known,
                 DeviceFn online, OwnerChangeFn on_owner_change);

  // システムモードとの連携（Phase 4。Core が設定する）
  struct SystemHooks {
    std::string self_id;           // gw-01：この宛ての set_mode は承認（v0.2.3 #46）
    std::function<bool()> emergency;  // EMERGENCY なら schedule の request を拒否する
    // 承認：条件が回復していれば NORMAL に戻し、承認の処理を表す ULID を返す。回復していなければ
    // nullopt（unsafe）
    std::function<std::optional<std::string>()> approve;
    // 手動の release で所有を外された主張の主（rule:<id> なら Core がルールを黙らせる。#26）
    std::function<void(const std::string& owner)> released;
  };
  void set_system_hooks(SystemHooks hooks) { hooks_ = std::move(hooks); }
  // requests.yaml の再読込
  void set_rules(RequestRules rules) { rules_ = std::move(rules); }

  // request の payload を処理する。request/ack を acks に積み、指令は commands に出す
  void handle(const Json& request, const std::string& requester, SteadyTime now,
              std::vector<RequestAck>& acks, CommandOutputs& cmd_out);
  // 指令が終端に達した：対応する request の最終結果を返す
  void on_command_finished(const CommandRecord& cmd, SteadyTime now, std::vector<RequestAck>& acks,
                           CommandOutputs& cmd_out);
  // 1時間を過ぎた request_id を忘れる
  void purge(SteadyTime now);

  [[nodiscard]] const RequestRecord* find(const std::string& request_id) const;
  [[nodiscard]] const RequestStats& stats() const { return stats_; }
  [[nodiscard]] std::size_t remembered() const { return records_.size(); }
  [[nodiscard]] std::optional<SteadyTime> next_purge() const;

 private:
  void respond(const RequestRecord& r, std::vector<RequestAck>& acks);
  void reject(RequestRecord& r, std::string reason, std::vector<RequestAck>& acks);
  void approve(RequestRecord& r, const Json& request, std::vector<RequestAck>& acks);

  RequestRules rules_;
  Arbiter& arbiter_;
  CommandManager& commands_;
  DeviceFn known_;
  DeviceFn online_;
  OwnerChangeFn on_owner_change_;
  SystemHooks hooks_;
  RequestStats stats_;
  std::map<std::string, RequestRecord> records_;
  std::map<std::string, std::string> by_cmd_;  // cmd_id → request_id
};

}  // namespace gw::core::command
