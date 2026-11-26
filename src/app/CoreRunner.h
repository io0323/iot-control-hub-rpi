// core のスレッド（詳細設計書 第4章の CoreRunner）。
//   while: msg = inbound.pop_until(core.next_deadline())（最大1秒で切り上げる）
//          out = step(msg) / on_timer(now)、dispatch(out)（各キューに try_push）
// inbound が閉じられて空になったら、core.shutdown()（gw-01 の offline の state）を送り、
// mqtt-out と uart-out を閉じて終わる。
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

#include "core/Core.h"
#include "core/common/BoundedQueue.h"
#include "core/common/Clock.h"

namespace gw::app {

struct CoreRunnerStats {
  std::uint64_t heartbeat_skipped = 0;  // uart-out が満杯で飛ばした HEARTBEAT
  std::uint64_t cmd_requeued = 0;       // uart-out が満杯で送信待ちに戻した CMD
  std::uint64_t mqtt_evicted = 0;       // mqtt-out から追い出した telemetry
  std::uint64_t mqtt_dropped = 0;  // mqtt-out に入らなかった電文（DATA_DROPPED の対象）
};

class CoreRunner {
 public:
  CoreRunner(core::Core& core, const core::IClock& clock,
             core::BoundedQueue<core::Inbound>& inbound, core::BoundedQueue<core::UartTx>& uart_out,
             core::BoundedQueue<core::MqttTx>& mqtt_out, std::function<void()> uart_notify);

  void run();
  [[nodiscard]] CoreRunnerStats stats() const;
  // 統計のログ（core のスレッドで kStatsInterval ごとと終了時に呼ぶ。他のスレッドの統計も読める）
  void on_stats(std::function<void(const core::Core&, bool final)> handler) {
    stats_handler_ = std::move(handler);
  }
  static constexpr std::chrono::seconds kStatsInterval{60};  // open-questions #15（io の決定）

 private:
  void dispatch(core::Outputs out);

  core::Core& core_;
  const core::IClock& clock_;
  core::BoundedQueue<core::Inbound>& inbound_;
  core::BoundedQueue<core::UartTx>& uart_out_;
  core::BoundedQueue<core::MqttTx>& mqtt_out_;
  std::function<void()> uart_notify_;
  std::function<void(const core::Core&, bool)> stats_handler_;
  mutable std::mutex stats_mutex_;
  CoreRunnerStats stats_;
};

}  // namespace gw::app
