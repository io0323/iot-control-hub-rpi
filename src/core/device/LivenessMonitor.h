// 生存監視（基本設計 FR-04、open-questions #4）。
// - UART：最後のフレーム受信から2秒で suspect、3秒で offline。STATE を受けたら online。切断で offline
// - MQTT：state の online で online、LWT または state の offline で offline。
//   state のどのチャネルも stale になったら suspect（チャネルを持たないデバイスは suspect にならない）。
//   suspect の間に新しい値が届いたら online に戻る
// - suspect ではイベントを出さない。online・suspect ⇔ offline の変化で DEVICE_ONLINE／DEVICE_OFFLINE を
//   1件ずつ出す。まだ一度も観測していないデバイスが最初に online になったときは出さない（gw_01_lite と同じ）
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/common/Clock.h"
#include "core/device/DeviceRegistry.h"

namespace gw::core::device {

struct LivenessChange {
  std::string device_id;
  std::optional<DeviceStatus> from;
  DeviceStatus to;
  // 発行するイベント（DEVICE_ONLINE・DEVICE_OFFLINE）。出さない変化なら空
  std::string event_code;
};

class LivenessMonitor {
 public:
  static constexpr std::chrono::seconds kUartSuspectAfter{2};
  static constexpr std::chrono::seconds kUartOfflineAfter{3};

  explicit LivenessMonitor(DeviceRegistry& registry) : registry_(registry) {}

  // 状態を変える。変わらなければ nullopt
  std::optional<LivenessChange> set_status(const std::string& device_id, DeviceStatus to);

  // ---- UART（復号できたフレームの受信で呼ぶ。最初の受信までは数えない。open-questions #3）
  void uart_frame(const std::string& device_id, SteadyTime now);
  // 期限が来たか調べる（suspect・offline への変化）。now 以降の次の期限も返す
  std::vector<LivenessChange> uart_check(SteadyTime now);
  [[nodiscard]] std::optional<SteadyTime> uart_next_deadline() const;

  // ---- MQTT
  // 1秒ごと：チャネルの stale を更新し、すべて stale なら suspect にする
  std::vector<LivenessChange> stale_check(SteadyTime now);
  // 新しい値を受けたあと：suspect なら online に戻す
  std::optional<LivenessChange> fresh_value(const std::string& device_id);

 private:
  DeviceRegistry& registry_;
  std::map<std::string, SteadyTime> uart_last_frame_;
};

}  // namespace gw::core::device
