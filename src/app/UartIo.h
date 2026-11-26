// uart-io スレッド（詳細設計書 第3章・第5章）。fd と uart-out の起床用パイプを poll する。
// バイト列とフレームの相互変換（COBS・CRC）だけを行い、
// 受信時刻（system_now）を付けて inbound に入れる。
// 切断（EIO・ENXIO・0 バイト）でポートを閉じて UartLinkDown を送り、
// 指数バックオフ（1・2・4…30秒）で開き直す。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "adapters/serial/SerialPort.h"
#include "core/Core.h"
#include "core/common/BoundedQueue.h"
#include "core/common/Clock.h"
#include "core/uart/FrameCodec.h"

namespace gw::app {

struct UartIoStats {
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t dropped_while_closed = 0;
  std::uint64_t evicted_telemetry = 0;
  std::uint64_t opens = 0;
  std::uint64_t disconnects = 0;
};

class UartIo {
 public:
  static constexpr std::chrono::seconds kBackoffMin{1};
  static constexpr std::chrono::seconds kBackoffMax{30};

  UartIo(std::string path, int baud, core::BoundedQueue<core::Inbound>& inbound,
         core::BoundedQueue<core::UartTx>& uart_out, const core::IClock& clock);
  ~UartIo();
  UartIo(const UartIo&) = delete;
  UartIo& operator=(const UartIo&) = delete;
  UartIo(UartIo&&) = delete;
  UartIo& operator=(UartIo&&) = delete;

  void start();
  // 受信を止めてスレッドを終える
  void stop();
  // uart-out に入れたことを知らせる（poll を起こす）
  void notify() const;
  // バックオフを待たずにポートを開き直す（ルールの reconnect、open-questions #32）
  void reconnect();

  [[nodiscard]] UartIoStats stats() const;

 private:
  void run();
  void wait_backoff(std::chrono::milliseconds delay);
  void drain_wake_pipe() const;
  void take_outgoing(bool port_open);
  void handle_read();
  void handle_write();
  void disconnect();

  std::string path_;
  int baud_;
  core::BoundedQueue<core::Inbound>& inbound_;
  core::BoundedQueue<core::UartTx>& uart_out_;
  const core::IClock& clock_;
  adapters::SerialPort port_;
  core::uart::FrameReader reader_;
  core::uart::Bytes write_buffer_;
  int wake_read_ = -1;
  int wake_write_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> reconnect_{false};
  std::thread thread_;
  mutable std::mutex stats_mutex_;
  UartIoStats stats_;
};

}  // namespace gw::app
