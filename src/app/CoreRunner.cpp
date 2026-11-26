#include "app/CoreRunner.h"

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>

#include "core/common/Topics.h"

namespace gw::app {

namespace {
// pop_until の待ちは最大1秒で切り上げる（第4章「ウォッチドッグ」）
constexpr std::chrono::seconds kMaxWait{1};
}  // namespace

CoreRunner::CoreRunner(core::Core& core, const core::IClock& clock,
                       core::BoundedQueue<core::Inbound>& inbound,
                       core::BoundedQueue<core::UartTx>& uart_out,
                       core::BoundedQueue<core::MqttTx>& mqtt_out,
                       std::function<void()> uart_notify)
    : core_(core),
      clock_(clock),
      inbound_(inbound),
      uart_out_(uart_out),
      mqtt_out_(mqtt_out),
      uart_notify_(std::move(uart_notify)) {
}

CoreRunnerStats CoreRunner::stats() const {
  const std::lock_guard lock(stats_mutex_);
  return stats_;
}

void CoreRunner::run() {
  dispatch(core_.start());
  auto next_stats = clock_.steady_now() + kStatsInterval;
  for (;;) {
    auto deadline = clock_.steady_now() + kMaxWait;
    if (const auto next = core_.next_deadline()) {
      deadline = std::min(deadline, *next);
    }
    auto msg = inbound_.pop_until(deadline);
    if (msg) {
      dispatch(core_.step(*msg));
    } else if (inbound_.closed() && inbound_.size() == 0) {
      break;
    }
    dispatch(core_.on_timer(clock_.steady_now()));
    if (stats_handler_ && clock_.steady_now() >= next_stats) {
      stats_handler_(core_, false);
      next_stats = clock_.steady_now() + kStatsInterval;
    }
  }
  dispatch(core_.shutdown());
  if (stats_handler_) {
    stats_handler_(core_, true);
  }
  mqtt_out_.close();
  uart_out_.close();
}

void CoreRunner::dispatch(core::Outputs out) {
  for (const auto& line : out.logs) {
    spdlog::info("{}", line);
  }
  if ((out.reconnect_uart || out.reconnect_mqtt) && reconnect_handler_) {
    reconnect_handler_(out.reconnect_uart, out.reconnect_mqtt);
  }
  bool uart_pushed = false;
  for (auto& tx : out.uart) {
    const bool is_cmd = std::holds_alternative<core::uart::Cmd>(tx.frame);
    core::uart::Cmd cmd;
    if (is_cmd) {
      cmd = std::get<core::uart::Cmd>(tx.frame);
    }
    if (uart_out_.try_push(std::move(tx))) {
      uart_pushed = true;
      continue;
    }
    const std::lock_guard lock(stats_mutex_);
    if (is_cmd) {
      core_.requeue_uart_cmd(cmd);  // 入らない CMD は送信待ちに残す
      ++stats_.cmd_requeued;
    } else {
      ++stats_.heartbeat_skipped;  // HEARTBEAT はその回を飛ばす
    }
  }
  if (uart_pushed && uart_notify_) {
    uart_notify_();
  }
  for (auto& tx : out.mqtt) {
    // 満杯なら古い telemetry を追い出す。telemetry が無いときは新しい方を捨てる（第3章）
    const auto result = mqtt_out_.try_push_evicting(
        std::move(tx), [](const core::MqttTx& item) { return item.kind == core::kKindTelemetry; });
    const std::lock_guard lock(stats_mutex_);
    if (result == core::PushResult::kEvicted) {
      ++stats_.mqtt_evicted;
    } else if (result == core::PushResult::kRejected) {
      ++stats_.mqtt_dropped;
    }
  }
}

}  // namespace gw::app
