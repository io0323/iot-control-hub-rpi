// シリアルポート（詳細設計書 第5章「送信と接続」、第7章）。termios・cfmakeraw・非ブロッキング。
// read が EIO・ENXIO・0 バイト（相手が閉じた）を返したら切断とみなす（macOS と Linux
// で結果が違うため すべて切断とする）。pty では速度設定の失敗を無視する。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gw::adapters {

class SerialPort {
 public:
  enum class IoStatus : std::uint8_t {
    kOk,  // 読み書きできた（0 バイトのこともある：EAGAIN）
    kWouldBlock,
    kDisconnected,
  };

  SerialPort() = default;
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  SerialPort(SerialPort&& other) noexcept;
  SerialPort& operator=(SerialPort&& other) noexcept;
  ~SerialPort();

  // 開いて raw・8N1 に設定する。失敗したら false（error() に理由）
  bool open(const std::string& path, int baud);
  void close();
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }
  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] const std::string& error() const { return error_; }

  // 読めるだけ読む（非ブロッキング）
  IoStatus read_some(std::vector<std::uint8_t>& out);
  // 書けるだけ書き、書いたバイト数を written に入れる
  IoStatus write_some(std::span<const std::uint8_t> data, std::size_t& written);

 private:
  int fd_ = -1;
  std::string error_;
};

}  // namespace gw::adapters
