// テスト用の pty の対（openpty）。master 側をテストが持ち、slave のパスを SerialPort に開かせる。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

class PtyPair {
 public:
  PtyPair() {
    int slave = -1;
    if (::openpty(&master_, &slave, nullptr, nullptr, nullptr) != 0) {
      throw std::runtime_error("openpty に失敗した");
    }
    termios tio{};
    ::tcgetattr(slave, &tio);
    ::cfmakeraw(&tio);
    ::tcsetattr(slave, TCSANOW, &tio);
    path_ = ::ttyname(slave);
    slave_ = slave;  // 閉じると相手が開く前の書き込みが消えるので持っておく
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ::fcntl(master_, F_SETFL, ::fcntl(master_, F_GETFL) | O_NONBLOCK);
  }
  ~PtyPair() { close_all(); }
  PtyPair(const PtyPair&) = delete;
  PtyPair& operator=(const PtyPair&) = delete;
  PtyPair(PtyPair&&) = delete;
  PtyPair& operator=(PtyPair&&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

  void write(const std::vector<std::uint8_t>& data) const {
    std::size_t done = 0;
    while (done < data.size()) {
      const auto n = ::write(master_, data.data() + done, data.size() - done);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
      }
    }
  }

  [[nodiscard]] std::vector<std::uint8_t> read_available() const {
    std::vector<std::uint8_t> out(4096);
    const auto n = ::read(master_, out.data(), out.size());
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
  }

  // 相手（作品01の役）がいなくなる
  void close_all() {
    for (int* fd : {&master_, &slave_}) {
      if (*fd >= 0) {
        ::close(*fd);
        *fd = -1;
      }
    }
  }

 private:
  int master_ = -1;
  int slave_ = -1;
  std::string path_;
};
