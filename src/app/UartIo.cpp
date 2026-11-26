#include "app/UartIo.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace gw::app {

namespace {

bool is_telemetry_inbound(const core::Inbound& msg) {
  const auto* rx = std::get_if<core::UartReceived>(&msg);
  if (rx == nullptr) {
    return false;
  }
  const auto* frame = std::get_if<core::uart::Frame>(&rx->decoded);
  return frame != nullptr && std::holds_alternative<core::uart::Telemetry>(*frame);
}

void set_nonblock_cloexec(int fd) {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
}

}  // namespace

UartIo::UartIo(std::string path, int baud, core::BoundedQueue<core::Inbound>& inbound,
               core::BoundedQueue<core::UartTx>& uart_out, const core::IClock& clock)
    : path_(std::move(path)), baud_(baud), inbound_(inbound), uart_out_(uart_out), clock_(clock) {
  std::array<int, 2> fds{};
  if (::pipe(fds.data()) != 0) {
    throw std::runtime_error("pipe に失敗した");
  }
  wake_read_ = fds[0];
  wake_write_ = fds[1];
  set_nonblock_cloexec(wake_read_);
  set_nonblock_cloexec(wake_write_);
}

UartIo::~UartIo() {
  stop();
  ::close(wake_read_);
  ::close(wake_write_);
}

void UartIo::start() {
  thread_ = std::thread([this] { run(); });
}

void UartIo::stop() {
  stopping_ = true;
  notify();
  if (thread_.joinable()) {
    thread_.join();
  }
  port_.close();
}

void UartIo::notify() const {
  const std::uint8_t byte = 1;
  (void)::write(wake_write_, &byte, 1);  // 満杯（EAGAIN）でも poll は起きる
}

UartIoStats UartIo::stats() const {
  const std::lock_guard lock(stats_mutex_);
  return stats_;
}

void UartIo::drain_wake_pipe() const {
  std::array<std::uint8_t, 64> buffer{};
  while (::read(wake_read_, buffer.data(), buffer.size()) > 0) {
  }
}

void UartIo::wait_backoff(std::chrono::milliseconds delay) {
  const auto deadline = std::chrono::steady_clock::now() + delay;
  while (!stopping_) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return;
    }
    pollfd pfd{wake_read_, POLLIN, 0};
    (void)::poll(&pfd, 1, static_cast<int>(left.count()));
    drain_wake_pipe();
    take_outgoing(false);  // 閉じている間に送ろうとしたものは捨てる
  }
}

void UartIo::take_outgoing(bool port_open) {
  while (auto item = uart_out_.try_pop()) {
    if (!port_open) {
      const std::lock_guard lock(stats_mutex_);
      ++stats_.dropped_while_closed;
      continue;
    }
    const auto bytes = core::uart::encode_frame(item->frame);
    write_buffer_.insert(write_buffer_.end(), bytes.begin(), bytes.end());
    const std::lock_guard lock(stats_mutex_);
    ++stats_.frames_out;
  }
}

void UartIo::disconnect() {
  port_.close();
  reader_.clear();
  write_buffer_.clear();
  {
    const std::lock_guard lock(stats_mutex_);
    ++stats_.disconnects;
  }
  spdlog::warn("event=uart_link_down path={}", path_);
  (void)inbound_.push(core::Inbound{core::UartLinkDown{}});
}

void UartIo::handle_read() {
  std::vector<std::uint8_t> data;
  const auto status = port_.read_some(data);
  const auto received_at = clock_.system_now();
  for (auto& decoded : reader_.feed(data)) {
    core::Inbound msg{core::UartReceived{std::move(decoded), received_at}};
    {
      const std::lock_guard lock(stats_mutex_);
      ++stats_.frames_in;
    }
    if (is_telemetry_inbound(msg)) {
      // 満杯なら古い telemetry を追い出す。telemetry が無ければ空くまで待つ（第3章）
      if (inbound_.push_evicting(std::move(msg), is_telemetry_inbound) ==
          core::PushResult::kEvicted) {
        const std::lock_guard lock(stats_mutex_);
        ++stats_.evicted_telemetry;
      }
    } else {
      (void)inbound_.push(std::move(msg));
    }
  }
  if (status == adapters::SerialPort::IoStatus::kDisconnected) {
    disconnect();
  }
}

void UartIo::handle_write() {
  if (write_buffer_.empty()) {
    return;
  }
  std::size_t written = 0;
  const auto status = port_.write_some(write_buffer_, written);
  write_buffer_.erase(write_buffer_.begin(),
                      write_buffer_.begin() + static_cast<std::ptrdiff_t>(written));
  if (status == adapters::SerialPort::IoStatus::kDisconnected) {
    disconnect();
  }
}

void UartIo::run() {
  std::chrono::milliseconds backoff = kBackoffMin;
  while (!stopping_) {
    if (!port_.is_open()) {
      if (port_.open(path_, baud_)) {
        backoff = kBackoffMin;
        {
          const std::lock_guard lock(stats_mutex_);
          ++stats_.opens;
        }
        spdlog::info("event=uart_link_up path={}", path_);
        (void)inbound_.push(core::Inbound{core::UartLinkUp{}});
      } else {
        spdlog::debug("event=uart_open_failed error=\"{}\" retry_ms={}", port_.error(),
                      backoff.count());
        wait_backoff(backoff);
        backoff = std::min<std::chrono::milliseconds>(backoff * 2, kBackoffMax);
        continue;
      }
    }
    std::array<pollfd, 2> fds{};
    fds[0] =
        pollfd{port_.fd(), static_cast<short>(POLLIN | (write_buffer_.empty() ? 0 : POLLOUT)), 0};
    fds[1] = pollfd{wake_read_, POLLIN, 0};
    const int rc = ::poll(fds.data(), fds.size(), -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("event=uart_poll_failed errno={}", errno);
      disconnect();
      continue;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      drain_wake_pipe();
      take_outgoing(true);
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      handle_read();
    }
    if (port_.is_open()) {
      handle_write();
    }
  }
}

}  // namespace gw::app
