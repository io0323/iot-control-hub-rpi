// デバイス台帳（基本設計 FR-01、詳細設計書 第2章 core/device）。
// 静的登録（devices.yaml）と、state の channels・actuators による自動登録。
// チャネルごとの最新値・品質・最終更新時刻、デバイスの状態、boot_id、bridged_by を持つ。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/uart/Profile.h"

namespace gw::core::device {

// デバイスの状態（FR-04）。unknown は「bridged_by のデバイスが offline なので状態不明」（第5章）
enum class DeviceStatus : std::uint8_t { kOnline, kSuspect, kOffline, kUnknown };

[[nodiscard]] std::string_view to_string(DeviceStatus status);

struct ChannelState {
  std::string channel;
  std::string unit;
  std::int64_t period_ms = 0;
  std::optional<double> value;
  std::string quality;  // good・stale・fault（発信元が付けたもの）
  std::optional<SteadyTime>
      last_update;  // 最後に値を受けた時刻（受信時刻。デバイスの時計は使わない）
  bool stale = false;  // 受信側の判定（最終更新から period_ms の3倍超）
};

struct DeviceInfo {
  std::string device_id;
  std::string transport;  // uart・mqtt
  bool is_static = false;
  // 自分の生存状態。nullopt は「まだ一度も観測していない」（このときは変化のイベントを出さない）
  std::optional<DeviceStatus> status;
  std::optional<std::string> boot_id;
  std::optional<std::string> bridged_by;
  std::optional<std::string> fw_version;
  std::optional<std::string> mode;
  std::map<std::string, ChannelState> channels;
  Json actuators = Json::array();
};

class DeviceRegistry {
 public:
  // devices.yaml の1台（プロファイルと同じ形）
  void register_static(const uart::Profile& profile);
  // state の受信で登録・更新する（自動登録）。新しく登録したら true
  bool apply_state(const std::string& device_id, const std::string& boot_id, const Json& payload);
  // telemetry の1チャネル。登録されていないチャネルなら false
  bool update_reading(const std::string& device_id, const std::string& channel, double value,
                      const std::string& quality, SteadyTime received_at);

  [[nodiscard]] DeviceInfo* find(const std::string& device_id);
  [[nodiscard]] const DeviceInfo* find(const std::string& device_id) const;
  [[nodiscard]] const std::map<std::string, DeviceInfo>& devices() const { return devices_; }
  [[nodiscard]] std::map<std::string, DeviceInfo>& devices() { return devices_; }
  // 外から見た状態：bridged_by のデバイスが offline なら unknown（イベントは出さない）
  [[nodiscard]] std::optional<DeviceStatus> effective_status(const std::string& device_id) const;

 private:
  DeviceInfo& entry(const std::string& device_id);

  std::map<std::string, DeviceInfo> devices_;
};

}  // namespace gw::core::device
