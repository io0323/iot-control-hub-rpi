#include "adapters/serial/SerialPort.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace gw::adapters {

namespace {

constexpr std::size_t kReadChunk = 4096;

bool speed_of(int baud, speed_t& out) {
  switch (baud) {
    case 9600:
      out = B9600;
      return true;
    case 19200:
      out = B19200;
      return true;
    case 38400:
      out = B38400;
      return true;
    case 57600:
      out = B57600;
      return true;
    case 115200:
      out = B115200;
      return true;
    case 230400:
      out = B230400;
      return true;
    default:
      return false;
  }
}

// strerror はスレッド安全でないので std::error_code の message を使う
std::string errno_text() {
  return std::error_code(errno, std::generic_category()).message();
}

}  // namespace

SerialPort::SerialPort(SerialPort&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), error_(std::move(other.error_)) {
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
    error_ = std::move(other.error_);
  }
  return *this;
}

SerialPort::~SerialPort() {
  close();
}

bool SerialPort::open(const std::string& path, int baud) {
  close();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    error_ = path + ": " + errno_text();
    return false;
  }
  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) {
    error_ = path + ": tcgetattr: " + errno_text();
    ::close(fd);
    return false;
  }
  ::cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);
  // VMIN=0 だとデータが無いときの read が 0 を返し、切断と区別できない。VMIN=1 なら非ブロッキングで
  // EAGAIN
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  speed_t speed{};
  if (speed_of(baud, speed)) {
    // pty では速度を設定できないことがあるので、失敗は無視する
    (void)::cfsetispeed(&tio, speed);
    (void)::cfsetospeed(&tio, speed);
  }
  if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
    error_ = path + ": tcsetattr: " + errno_text();
    ::close(fd);
    return false;
  }
  fd_ = fd;
  error_.clear();
  return true;
}

void SerialPort::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

SerialPort::IoStatus SerialPort::read_some(std::vector<std::uint8_t>& out) {
  if (fd_ < 0) {
    return IoStatus::kDisconnected;
  }
  std::array<std::uint8_t, kReadChunk> buffer{};
  for (;;) {
    const auto n = ::read(fd_, buffer.data(), buffer.size());
    if (n > 0) {
      out.insert(out.end(), buffer.begin(), buffer.begin() + n);
      if (static_cast<std::size_t>(n) < buffer.size()) {
        return IoStatus::kOk;
      }
      continue;
    }
    if (n == 0) {
      return IoStatus::kDisconnected;  // 相手が閉じた
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return out.empty() ? IoStatus::kWouldBlock : IoStatus::kOk;
    }
    // EIO・ENXIO（相手が閉じた）を含め、ほかの誤りもすべて切断として扱う
    error_ = errno_text();
    return IoStatus::kDisconnected;
  }
}

SerialPort::IoStatus SerialPort::write_some(std::span<const std::uint8_t> data,
                                            std::size_t& written) {
  written = 0;
  if (fd_ < 0) {
    return IoStatus::kDisconnected;
  }
  while (written < data.size()) {
    const auto n = ::write(fd_, data.data() + written, data.size() - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return IoStatus::kWouldBlock;
    }
    error_ = n < 0 ? errno_text() : "write が 0 を返した";
    return IoStatus::kDisconnected;
  }
  return IoStatus::kOk;
}

}  // namespace gw::adapters
