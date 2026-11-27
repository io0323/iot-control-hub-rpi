// 履歴DB（基本設計 第7章「履歴DB」、FR-13、詳細設計書 第4章「起動の手順」）。SQLite、WAL、
// synchronous=NORMAL。書くのは history スレッドだけ（読み取り専用の接続は admin スレッドの gwctl diag）。
//
// - スキーマは PRAGMA user_version で版を管理し、open で移行を順に適用する
//   v1：基本設計 第7章の8テーブル
//   v2：uart_diag（open-questions #1、v0.2.2 #37 の15バイトの各項目）、devices.actuators、ts の索引
// - write：まとめて1つのトランザクションで書く（失敗したら例外。呼び出し側が保持して再試行する）
// - purge：保持期間（90日）を過ぎた行を消す。devices と、system_mode_log の最後の1行は残す
// - restore：起動時に未完了の指令を FAILED（restart）で閉じ、直近の requests・最後のモード・台帳を返す
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/history/HistoryRecord.h"

struct sqlite3;

namespace gw::adapters {

class HistoryError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// 書き込み先（テストで失敗を注入できるようにする）
class IHistorySink {
 public:
  IHistorySink() = default;
  IHistorySink(const IHistorySink&) = delete;
  IHistorySink& operator=(const IHistorySink&) = delete;
  IHistorySink(IHistorySink&&) = delete;
  IHistorySink& operator=(IHistorySink&&) = delete;
  virtual ~IHistorySink() = default;
  virtual void write(const std::vector<core::history::HistoryRecord>& records) = 0;
  // cutoff_ms より古い行を消し、消した行数を返す
  virtual std::uint64_t purge(std::int64_t cutoff_ms) = 0;
};

// 起動時に core に渡すもの（詳細設計書 第4章、D5）
struct RestoredRequest {
  std::string request_id;
  std::string source;
  std::string kind;
  std::string result;
  std::string reason;
  std::string cmd_id;
  std::int64_t ts_ms = 0;
};
struct RestoreState {
  std::uint64_t commands_closed = 0;      // FAILED（restart）で閉じた未完了の指令
  std::vector<RestoredRequest> requests;  // 直近1時間
  std::optional<core::history::ModeRow> last_mode;
  std::vector<core::history::DeviceRow> devices;
};

class HistoryStore final : public IHistorySink {
 public:
  static constexpr int kSchemaVersion = 2;
  static constexpr std::int64_t kRequestWindowMs = 3'600'000;  // 共通仕様 第4章：1時間

  // 開いて移行を適用する。開けない・移行できないなら HistoryError
  explicit HistoryStore(const std::filesystem::path& path);
  ~HistoryStore() override;
  HistoryStore(const HistoryStore&) = delete;
  HistoryStore& operator=(const HistoryStore&) = delete;
  HistoryStore(HistoryStore&&) = delete;
  HistoryStore& operator=(HistoryStore&&) = delete;

  void write(const std::vector<core::history::HistoryRecord>& records) override;
  std::uint64_t purge(std::int64_t cutoff_ms) override;
  [[nodiscard]] RestoreState restore(std::int64_t now_ms);

  [[nodiscard]] int schema_version();
  // テスト・gwctl 用：SQL を1つ実行し、各行を文字列の列で返す
  [[nodiscard]] std::vector<std::vector<std::string>> query(const std::string& sql);

  // 読み取り専用の接続で、since_ms 以降に受けた device の DIAG を新しい順に返す（gwctl diag）
  [[nodiscard]] static std::vector<core::history::DiagRow> read_diags(
      const std::filesystem::path& path, const std::string& device_id, std::int64_t since_ms,
      int limit);

  // 移行の SQL（v1 から順に）。テストで古い版の DB を作るのに使う
  [[nodiscard]] static const std::vector<std::string>& migrations();

 private:
  void exec(const std::string& sql);
  void migrate();

  sqlite3* db_ = nullptr;
};

}  // namespace gw::adapters
