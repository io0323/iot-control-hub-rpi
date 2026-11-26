// uart-io（UartIo）：pty 越しにフレームを inbound に入れ、uart-out を書き出し、切断を知らせる
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "app/UartIo.h"
#include "core/uart/FrameCodec.h"
#include "pty_pair.h"

namespace u = gw::core::uart;
using gw::core::BoundedQueue;
using gw::core::Inbound;
using gw::core::UartTx;
using namespace std::chrono_literals;

namespace {

template <typename T>
bool is(const Inbound& msg) {
  return std::holds_alternative<T>(msg);
}

std::optional<Inbound> next(BoundedQueue<Inbound>& q) {
  return q.pop_until(std::chrono::steady_clock::now() + 2s);
}

}  // namespace

TEST(UartIo, ReceivesFramesWritesOutgoingAndReportsDisconnect) {
  PtyPair pty;
  gw::core::SystemClock clock;
  BoundedQueue<Inbound> inbound(64);
  BoundedQueue<UartTx> uart_out(8);
  gw::app::UartIo io(pty.path(), 115200, inbound, uart_out, clock);
  io.start();

  auto msg = next(inbound);
  ASSERT_TRUE(msg && is<gw::core::UartLinkUp>(*msg));

  // 受信：フレーム2件と壊れたフレーム
  auto data = u::encode_frame(u::Heartbeat{0, 10});
  const auto state = u::encode_frame(u::State{0, 0x0102, 1, 1000});
  data.insert(data.end(), state.begin(), state.end());
  data.insert(data.end(), {0x05, 0x11, 0x00});
  pty.write(data);
  for (int i = 0; i < 3; ++i) {
    msg = next(inbound);
    ASSERT_TRUE(msg && is<gw::core::UartReceived>(*msg));
  }
  EXPECT_TRUE(std::holds_alternative<u::Discarded>(std::get<gw::core::UartReceived>(*msg).decoded));

  // 送信：uart-out に入れて知らせる
  ASSERT_TRUE(uart_out.try_push(UartTx{u::Heartbeat{7, 1234}}));
  io.notify();
  std::vector<std::uint8_t> written;
  for (int i = 0; i < 100 && written.empty(); ++i) {
    written = pty.read_available();
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(written, u::encode_frame(u::Heartbeat{7, 1234}));

  // 切断
  pty.close_all();
  msg = next(inbound);
  ASSERT_TRUE(msg && is<gw::core::UartLinkDown>(*msg));
  io.stop();
  EXPECT_EQ(io.stats().disconnects, 1U);
}

TEST(UartIo, RetriesOpenUntilStopped) {
  gw::core::SystemClock clock;
  BoundedQueue<Inbound> inbound(4);
  BoundedQueue<UartTx> uart_out(4);
  gw::app::UartIo io("/dev/does-not-exist-gw", 115200, inbound, uart_out, clock);
  io.start();
  ASSERT_TRUE(uart_out.try_push(UartTx{u::Heartbeat{0, 1}}));
  io.notify();
  std::this_thread::sleep_for(100ms);
  const auto start = std::chrono::steady_clock::now();
  io.stop();  // バックオフの待ちから抜ける
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
  EXPECT_EQ(inbound.size(), 0U);
  EXPECT_EQ(io.stats().dropped_while_closed, 1U);
}

TEST(UartIo, ReconnectReopensAnOpenPortImmediately) {
  PtyPair pty;
  gw::core::SystemClock clock;
  BoundedQueue<Inbound> inbound(16);
  BoundedQueue<UartTx> uart_out(4);
  gw::app::UartIo io(pty.path(), 115200, inbound, uart_out, clock);
  io.start();
  auto msg = next(inbound);
  ASSERT_TRUE(msg && is<gw::core::UartLinkUp>(*msg));
  // ルールの reconnect：開いているポートを一度閉じ、待たずに開き直す（open-questions #32）
  io.reconnect();
  msg = next(inbound);
  ASSERT_TRUE(msg && is<gw::core::UartLinkDown>(*msg));
  msg = next(inbound);
  ASSERT_TRUE(msg && is<gw::core::UartLinkUp>(*msg));
  io.stop();
  EXPECT_EQ(io.stats().opens, 2U);
}

TEST(UartIo, ReconnectCutsTheBackoffShort) {
  gw::core::SystemClock clock;
  BoundedQueue<Inbound> inbound(4);
  BoundedQueue<UartTx> uart_out(4);
  gw::app::UartIo io("/dev/does-not-exist-gw", 115200, inbound, uart_out, clock);
  io.start();
  std::this_thread::sleep_for(50ms);  // バックオフの待ちに入る
  io.reconnect();                     // 待ちを切り上げて開き直しを試す（開けないので待ちに戻る）
  std::this_thread::sleep_for(50ms);
  const auto start = std::chrono::steady_clock::now();
  io.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}
