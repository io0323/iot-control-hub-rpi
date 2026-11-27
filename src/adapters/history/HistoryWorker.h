// history スレッド（詳細設計書 第3章の history キュー、基本設計 第7章・第8章）。
//
// - 書き込みは100件または最初の1件から200msの一括（HistoryBatcher）
// - 失敗したらメモリに最大1万件を保持して1秒ごとに再試行する（RetryBuffer：あふれたら #41 の順に捨てる）。
//   捨てた件数は on_dropped で知らせ（core が DATA_DROPPED を出す）、復旧後に「この期間の履歴は
//   欠けている」ことを system_mode_log に書く。制御は止めない
// - 保持期間90日を過ぎた行は、gateway.yaml の timezone の3時に消す
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "adapters/history/HistoryStore.h"
#include "core/common/BoundedQueue.h"
#include "core/common/Clock.h"
#include "core/history/RetryBuffer.h"

namespace gw::adapters {

struct HistoryStats {
  std::uint64_t written = 0;  // 書いた件数
  std::uint64_t batches = 0;
  std::uint64_t write_failures = 0;
  std::uint64_t dropped = 0;   // 上限で捨てた件数
  std::uint64_t buffered = 0;  // 書けずに保持している件数（今）
  std::uint64_t purged = 0;
  std::uint64_t gaps = 0;  // system_mode_log に書いた「欠けた期間」
};

class HistoryBatcher {
 public:
  static constexpr std::size_t kBatchSize = 100;
  static constexpr std::chrono::milliseconds kBatchInterval{200};
  static constexpr std::chrono::milliseconds kRetryInterval{1000};

  explicit HistoryBatcher(IHistorySink& sink,
                          std::size_t capacity = core::history::RetryBuffer::kDefaultCapacity,
                          std::string initial_mode = "NORMAL");

  void add(core::history::HistoryRecord r, core::SteadyTime now);
  // 書く時刻なら書く：100件以上、最初の1件から200ms、失敗後の再試行の時刻
  void poll(core::SteadyTime now, std::int64_t now_ms);
  // 残りをすべて書こうとする（終了時）
  void flush(core::SteadyTime now, std::int64_t now_ms);
  [[nodiscard]] std::optional<core::SteadyTime> next_deadline() const;
  // 前回から増えた、捨てた件数（DATA_DROPPED 用）
  [[nodiscard]] std::uint64_t take_new_drops();
  [[nodiscard]] HistoryStats stats() const;
  [[nodiscard]] bool failing() const { return failing_; }

 private:
  void write_out(core::SteadyTime now, std::int64_t now_ms);

  IHistorySink& sink_;
  core::history::RetryBuffer buffer_;
  std::optional<core::SteadyTime> first_at_;
  std::optional<core::SteadyTime> retry_at_;
  bool failing_ = false;
  std::int64_t gap_from_ms_ = 0;
  std::uint64_t dropped_reported_ = 0;
  std::uint64_t dropped_at_gap_start_ = 0;
  std::string mode_;
  HistoryStats stats_;
};

// now の後で、現地時刻（UTC との差 offset_min 分）が hour 時ちょうどになる最初の時刻
[[nodiscard]] core::SystemTime next_daily(core::SystemTime now, int hour, int offset_min);

class HistoryWorker {
 public:
  static constexpr std::int64_t kRetentionMs = 90LL * 24 * 3600 * 1000;  // 90日
  static constexpr int kPurgeHour = 3;

  HistoryWorker(IHistorySink& sink, core::BoundedQueue<core::history::HistoryRecord>& queue,
                const core::IClock& clock, std::function<int(core::SystemTime)> utc_offset,
                std::function<void(std::uint64_t)> on_dropped, std::string initial_mode);

  // キューが閉じられて空になるまで書く（スレッドの本体）
  void run();
  [[nodiscard]] HistoryStats stats() const;

 private:
  IHistorySink& sink_;
  core::BoundedQueue<core::history::HistoryRecord>& queue_;
  const core::IClock& clock_;
  std::function<int(core::SystemTime)> utc_offset_;
  std::function<void(std::uint64_t)> on_dropped_;
  HistoryBatcher batcher_;
  mutable std::mutex stats_mutex_;
  HistoryStats stats_;
};

}  // namespace gw::adapters
