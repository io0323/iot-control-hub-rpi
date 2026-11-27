// gwctl の管理ソケット（基本設計 FR-15、Phase 5）。UNIX ドメインソケット（gateway.yaml の admin.socket、
// 権限 0660）で、1行1つの JSON の要求と応答をやり取りする。
//
//   要求：{"cmd": "devices|rules|stats|approve|reload|cmd|diag", "args": {...}}
//   応答：{"ok": true, "result": ...} または {"ok": false, "error": "..."}
//
// - devices・rules・stats・approve・cmd・diag は inbound に管理要求（core::AdminRequest）として入れ、
//   core の応答（core::AdminReply）を待つ。cmd は指令の最終結果まで最大15秒（open-questions #46）
// - reload は app の再読込（SIGHUP と同じ）を呼ぶ
// - diag は core が GET_DIAG を送ったあと、履歴DB を読み取り専用で読み、要求の後に届いた DIAG を返す
//   （最大5秒。core は実行中に DB を読まない：D5、open-questions #47）
// - stats は core の応答に、core の外の数（history スレッドなど）を足す
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "core/Core.h"

namespace gw::adapters {

// 1行の要求を解析する。誤りは文字列
[[nodiscard]] std::variant<core::AdminRequest, std::string> parse_admin_request(
    const std::string& line);

class AdminServer {
 public:
  static constexpr std::chrono::seconds kCmdTimeout{15};
  static constexpr std::chrono::seconds kReplyTimeout{3};
  static constexpr std::chrono::seconds kDiagWait{5};

  struct Hooks {
    std::function<bool(core::AdminRequest)> submit;  // inbound に入れる
    std::function<core::Json()> reload;              // 再読込（{ok, ...}）
    // device の since_ms 以降の DIAG（新しい順の配列）
    std::function<core::Json(const std::string& device, std::int64_t since_ms)> read_diags;
    std::function<core::Json()> extra_stats;  // stats に足すもの
  };

  AdminServer(std::filesystem::path socket, Hooks hooks);
  ~AdminServer();
  AdminServer(const AdminServer&) = delete;
  AdminServer& operator=(const AdminServer&) = delete;
  AdminServer(AdminServer&&) = delete;
  AdminServer& operator=(AdminServer&&) = delete;

  // ソケットを作ってスレッドを始める。作れなければ false（理由は error()。open-questions #48）
  [[nodiscard]] bool start();
  void stop();
  // core の応答（CoreRunner のスレッドから呼ぶ）
  void deliver(const core::AdminReply& reply);
  // 1つの要求を処理して応答を返す（テストからも使う）
  [[nodiscard]] core::Json handle(const std::string& line);

  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  void run();
  void serve(int fd);
  [[nodiscard]] std::optional<core::Json> ask(core::AdminRequest request,
                                              std::chrono::milliseconds timeout);

  std::filesystem::path socket_;
  Hooks hooks_;
  int listen_fd_ = -1;
  int wake_read_ = -1;
  int wake_write_ = -1;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::string error_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::uint64_t next_id_ = 1;
  std::map<std::uint64_t, std::optional<core::Json>> pending_;
};

}  // namespace gw::adapters
