// SerialPort：openpty の対で送受信と切断を確かめる
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <poll.h>

#include "adapters/serial/SerialPort.h"
#include "pty_pair.h"

using gw::adapters::SerialPort;
using namespace std::chrono_literals;

namespace {

void wait_readable(int fd) {
  pollfd pfd{fd, POLLIN, 0};
  (void)::poll(&pfd, 1, 1000);
}

}  // namespace

TEST(SerialPort, SendsAndReceivesOverPty) {
  PtyPair pty;
  SerialPort port;
  ASSERT_TRUE(port.open(pty.path(), 115200)) << port.error();
  const std::vector<std::uint8_t> to_port{0x01, 0x00, 0x02, 0x0A, 0x0D, 0x11, 0x13};
  pty.write(to_port);
  wait_readable(port.fd());
  std::vector<std::uint8_t> got;
  EXPECT_EQ(port.read_some(got), SerialPort::IoStatus::kOk);
  EXPECT_EQ(got, to_port);  // raw：0x00・改行・XON/XOFF をそのまま通す

  std::vector<std::uint8_t> empty;
  EXPECT_EQ(port.read_some(empty),
            SerialPort::IoStatus::kWouldBlock);  // データが無いのは切断ではない

  const std::vector<std::uint8_t> to_pty{0x00, 0xFF, 0x0A};
  std::size_t written = 0;
  EXPECT_EQ(port.write_some(to_pty, written), SerialPort::IoStatus::kOk);
  EXPECT_EQ(written, to_pty.size());
  std::vector<std::uint8_t> echoed;
  for (int i = 0; i < 50 && echoed.size() < to_pty.size(); ++i) {
    const auto chunk = pty.read_available();
    echoed.insert(echoed.end(), chunk.begin(), chunk.end());
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(echoed, to_pty);
}

TEST(SerialPort, DetectsDisconnectWhenPeerCloses) {
  PtyPair pty;
  SerialPort port;
  ASSERT_TRUE(port.open(pty.path(), 115200)) << port.error();
  pty.close_all();
  wait_readable(port.fd());
  std::vector<std::uint8_t> got;
  EXPECT_EQ(port.read_some(got), SerialPort::IoStatus::kDisconnected);
}

TEST(SerialPort, OpenFailsForMissingPath) {
  SerialPort port;
  EXPECT_FALSE(port.open("/dev/does-not-exist-gw", 115200));
  EXPECT_FALSE(port.error().empty());
  EXPECT_FALSE(port.is_open());
}
