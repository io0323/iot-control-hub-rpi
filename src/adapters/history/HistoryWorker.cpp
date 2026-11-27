#include "adapters/history/HistoryWorker.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace gw::adapters {

namespace h = core::history;

namespace {

constexpr std::int64_t kMsPerMinute = 60'000;
constexpr std::int64_t kMsPerDay = 24LL * 60 * kMsPerMinute;
constexpr std::int64_t kMsPerHour = 60 * kMsPerMinute;

std::int64_t to_ms(core::SystemTime t) {
  return t.time_since_epoch().count();
}

}  // namespace

HistoryBatcher::HistoryBatcher(IHistorySink& sink, std::size_t capacity, std::string initial_mode)
    : sink_(sink), buffer_(capacity), mode_(std::move(initial_mode)) {
}

void HistoryBatcher::add(h::HistoryRecord r, core::SteadyTime now) {
  if (const auto* m = std::get_if<h::ModeRow>(&r.row)) {
    mode_ = m->to_mode;  // 欠けた期間を書くときのモード
  }
  if (buffer_.empty() && !first_at_) {
    first_at_ = now;
  }
  (void)buffer_.push(std::move(r));
}

void HistoryBatcher::poll(core::SteadyTime now, std::int64_t now_ms) {
  if (buffer_.empty()) {
    first_at_.reset();
    return;
  }
  if (failing_) {
    if (retry_at_ && now >= *retry_at_) {
      write_out(now, now_ms);
    }
    return;
  }
  if (buffer_.size() >= kBatchSize || (first_at_ && now - *first_at_ >= kBatchInterval)) {
    write_out(now, now_ms);
  }
}

void HistoryBatcher::flush(core::SteadyTime now, std::int64_t now_ms) {
  if (!buffer_.empty()) {
    write_out(now, now_ms);
  }
}

void HistoryBatcher::write_out(core::SteadyTime now, std::int64_t now_ms) {
  auto chunk = buffer_.take(buffer_.size());
  try {
    sink_.write(chunk);
  } catch (const std::exception& e) {
    ++stats_.write_failures;
    if (!failing_) {
      failing_ = true;
      gap_from_ms_ = now_ms;
      dropped_at_gap_start_ = buffer_.dropped();
      spdlog::error("event=history_write_failed buffered={} error=\"{}\"", chunk.size(), e.what());
    }
    buffer_.restore(std::move(chunk));  // 保持して再試行する（あふれたら #41 の順に捨てる）
    retry_at_ = now + kRetryInterval;
    return;
  }
  stats_.written += chunk.size();
  ++stats_.batches;
  first_at_.reset();
  if (failing_) {
    failing_ = false;
    retry_at_.reset();
    const auto lost = buffer_.dropped() - dropped_at_gap_start_;
    spdlog::info("event=history_write_recovered dropped={}", lost);
    if (lost > 0) {
      // 復旧後に「この期間の履歴は欠けている」ことを system_mode_log に書く（基本設計 第8章）
      const h::ModeRow gap{
          mode_, mode_,
          "history_gap from=" + core::format_ts(core::SystemTime{core::Millis{gap_from_ms_}}) +
              " to=" + core::format_ts(core::SystemTime{core::Millis{now_ms}}) +
              " dropped=" + std::to_string(lost),
          now_ms};
      try {
        sink_.write({h::record(gap)});
        ++stats_.gaps;
      } catch (const std::exception& e) {
        (void)buffer_.push(h::record(gap));  // 次の書き込みで書く
        spdlog::error("event=history_gap_write_failed error=\"{}\"", e.what());
      }
    }
  }
}

std::optional<core::SteadyTime> HistoryBatcher::next_deadline() const {
  if (buffer_.empty()) {
    return std::nullopt;
  }
  if (failing_) {
    return retry_at_;
  }
  return first_at_ ? std::optional{*first_at_ + kBatchInterval} : std::nullopt;
}

std::uint64_t HistoryBatcher::take_new_drops() {
  const auto now = buffer_.dropped();
  const auto delta = now - dropped_reported_;
  dropped_reported_ = now;
  return delta;
}

HistoryStats HistoryBatcher::stats() const {
  HistoryStats s = stats_;
  s.dropped = buffer_.dropped();
  s.buffered = failing_ ? buffer_.size() : 0;
  return s;
}

core::SystemTime next_daily(core::SystemTime now, int hour, int offset_min) {
  const auto offset_ms = static_cast<std::int64_t>(offset_min) * kMsPerMinute;
  const auto local = to_ms(now) + offset_ms;
  const auto day_start = local - (((local % kMsPerDay) + kMsPerDay) % kMsPerDay);
  auto target = day_start + hour * kMsPerHour;
  if (target <= local) {
    target += kMsPerDay;
  }
  return core::SystemTime{core::Millis{target - offset_ms}};
}

HistoryWorker::HistoryWorker(IHistorySink& sink, core::BoundedQueue<h::HistoryRecord>& queue,
                             const core::IClock& clock,
                             std::function<int(core::SystemTime)> utc_offset,
                             std::function<void(std::uint64_t)> on_dropped,
                             std::string initial_mode)
    : sink_(sink),
      queue_(queue),
      clock_(clock),
      utc_offset_(std::move(utc_offset)),
      on_dropped_(std::move(on_dropped)),
      batcher_(sink, h::RetryBuffer::kDefaultCapacity, std::move(initial_mode)) {
}

void HistoryWorker::run() {
  const auto offset = [&] { return utc_offset_ ? utc_offset_(clock_.system_now()) : 0; };
  auto next_purge = next_daily(clock_.system_now(), kPurgeHour, offset());
  while (true) {
    // 次に起きる時刻：一括の期限・再試行・保持期間の削除（3時）のうち早いもの。最大1秒
    auto deadline = clock_.steady_now() + std::chrono::seconds{1};
    if (const auto d = batcher_.next_deadline(); d && *d < deadline) {
      deadline = *d;
    }
    auto item = queue_.pop_until(deadline);
    const bool closed = !item && queue_.closed() && queue_.size() == 0;
    if (item) {
      batcher_.add(std::move(*item), clock_.steady_now());
      while (auto more = queue_.try_pop()) {  // 届いている分をまとめて受ける
        batcher_.add(std::move(*more), clock_.steady_now());
      }
    }
    const auto now_sys = clock_.system_now();
    batcher_.poll(clock_.steady_now(), to_ms(now_sys));
    if (now_sys >= next_purge) {
      try {
        const auto purged = sink_.purge(to_ms(now_sys) - kRetentionMs);
        spdlog::info("event=history_purged rows={}", purged);
        const std::lock_guard lock(stats_mutex_);
        stats_.purged += purged;
      } catch (const std::exception& e) {
        spdlog::error("event=history_purge_failed error=\"{}\"", e.what());
      }
      next_purge = next_daily(now_sys, kPurgeHour, offset());
    }
    if (const auto drops = batcher_.take_new_drops(); drops > 0 && on_dropped_) {
      on_dropped_(drops);
    }
    {
      const std::lock_guard lock(stats_mutex_);
      const auto purged = stats_.purged;
      stats_ = batcher_.stats();
      stats_.purged = purged;
    }
    if (closed) {
      batcher_.flush(clock_.steady_now(), to_ms(clock_.system_now()));  // 書き切る（第3章「終了」）
      const std::lock_guard lock(stats_mutex_);
      const auto purged = stats_.purged;
      stats_ = batcher_.stats();
      stats_.purged = purged;
      return;
    }
  }
}

HistoryStats HistoryWorker::stats() const {
  const std::lock_guard lock(stats_mutex_);
  return stats_;
}

}  // namespace gw::adapters
