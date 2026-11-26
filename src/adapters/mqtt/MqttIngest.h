// MQTT の受信（詳細設計書 第6章「受信（mqtt-loop）」）。mqtt-loop のスレッドで呼ばれる。
// - パースとスキーマ検証だけを行い、MqttReceived を inbound に入れる。不正なものは理由ごとに数えて捨てる
// - 同じ発信元のログは10秒に1回に抑える
// - 自分（gw-01）と bridged のデバイス（UART のデバイス）が発行した電文は捨てる（購読で戻ってくるため）
// - request は Phase 3 まで数えて捨てるだけ
// 重複・stale・欠番の判定は core。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "adapters/mqtt/IMqttClient.h"
#include "adapters/mqtt/SchemaValidator.h"
#include "core/Core.h"
#include "core/common/Clock.h"

namespace gw::adapters {

struct IngestStats {
  std::uint64_t received = 0;
  std::uint64_t accepted = 0;
  std::uint64_t own_echo = 0;         // 自分・bridged のデバイスの電文
  std::uint64_t request_ignored = 0;  // request（Phase 3 で扱う）
  std::uint64_t inbound_dropped = 0;  // inbound に200ms で入らなかった
  std::map<std::string, std::uint64_t>
      rejected;  // 理由ごと（json_invalid・unknown_schema・schema_invalid）
  std::chrono::nanoseconds validate_time{0};  // 検証にかかった時間の合計（Q7）
  std::uint64_t validated = 0;
};

class MqttIngest {
 public:
  static constexpr std::chrono::milliseconds kInboundWait{200};  // 第3章：mqtt-loop は最大200ms待つ
  static constexpr std::chrono::seconds kLogInterval{10};  // 同じ発信元のログは10秒に1回

  // push は inbound への投入（待ちは最大 kInboundWait）。入らなければ false
  MqttIngest(const SchemaValidator& validator, std::set<std::string> own_devices,
             const core::IClock& clock, std::function<bool(core::Inbound)> push);

  void handle(const MqttMessage& message);
  [[nodiscard]] IngestStats stats() const;
  // 購読するトピック（第6章「接続」）
  [[nodiscard]] static std::vector<std::string> subscriptions(const std::string& site);

 private:
  void log_rejected(const std::string& topic, const std::string& reason, const std::string& detail);

  const SchemaValidator& validator_;
  std::set<std::string> own_devices_;
  const core::IClock& clock_;
  std::function<bool(core::Inbound)> push_;
  mutable std::mutex mutex_;
  IngestStats stats_;
  std::map<std::string, core::SteadyTime> last_log_;
};

}  // namespace gw::adapters
