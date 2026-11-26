// 有界キュー（詳細設計書 第3章）。スレッド間の受け渡しに使う。
// - push：満杯なら空くまで待つ（受信スレッド → core の向きだけで使う）
// - push_for：待つのを最大 timeout まで（mqtt-loop は200ms）
// - try_push：待たない（core から先はすべてこれ。D4）
// - push_evicting / try_push_evicting：満杯なら evictable を満たす最も古い要素を追い出して入れる
// - pop_until：deadline まで待つ（core のループ）。close 後は残りを返してから nullopt
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace gw::core {

enum class PushResult {
  kPushed,   // そのまま入った
  kEvicted,  // 古い要素を1件追い出して入った
  kRejected,  // 入らなかった（満杯で追い出せるものがない、または閉じている）
};

template <typename T>
class BoundedQueue {
 public:
  using Clock = std::chrono::steady_clock;

  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  // 満杯なら待つ。閉じていれば false
  bool push(T item) {
    std::unique_lock lock(mutex_);
    not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    return push_locked(lock, std::move(item));
  }

  bool push_for(T item, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!not_full_.wait_for(lock, timeout, [&] { return closed_ || items_.size() < capacity_; })) {
      return false;
    }
    return push_locked(lock, std::move(item));
  }

  bool try_push(T item) {
    std::unique_lock lock(mutex_);
    if (items_.size() >= capacity_) {
      return false;
    }
    return push_locked(lock, std::move(item));
  }

  // 満杯なら追い出せる最も古い要素を追い出す。追い出せるものがなければ空くまで待つ
  template <typename Pred>
  PushResult push_evicting(T item, Pred evictable) {
    std::unique_lock lock(mutex_);
    if (closed_) {
      return PushResult::kRejected;
    }
    if (items_.size() >= capacity_ && evict_locked(evictable)) {
      items_.push_back(std::move(item));
      lock.unlock();
      not_empty_.notify_one();
      return PushResult::kEvicted;
    }
    not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    return push_locked(lock, std::move(item)) ? PushResult::kPushed : PushResult::kRejected;
  }

  // 待たない版。追い出せなければ新しい方（item）を捨てる
  template <typename Pred>
  PushResult try_push_evicting(T item, Pred evictable) {
    std::unique_lock lock(mutex_);
    if (closed_) {
      return PushResult::kRejected;
    }
    PushResult result = PushResult::kPushed;
    if (items_.size() >= capacity_) {
      if (!evict_locked(evictable)) {
        return PushResult::kRejected;
      }
      result = PushResult::kEvicted;
    }
    items_.push_back(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return result;
  }

  // 要素が来るまで待つ。閉じていて空なら nullopt
  std::optional<T> pop() { return pop_until(std::nullopt); }

  // deadline まで待つ。時間切れ・閉じていて空なら nullopt
  std::optional<T> pop_until(std::optional<Clock::time_point> deadline) {
    std::unique_lock lock(mutex_);
    const auto ready = [&] { return closed_ || !items_.empty(); };
    if (deadline) {
      not_empty_.wait_until(lock, *deadline, ready);
    } else {
      not_empty_.wait(lock, ready);
    }
    if (items_.empty()) {
      return std::nullopt;
    }
    T item = std::move(items_.front());
    items_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return item;
  }

  std::optional<T> try_pop() { return pop_until(Clock::time_point::min()); }

  // 以後の push を断り、待っている側を起こす。残っている要素は pop できる
  void close() {
    {
      const std::lock_guard lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    const std::lock_guard lock(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const {
    const std::lock_guard lock(mutex_);
    return items_.size();
  }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }

 private:
  bool push_locked(std::unique_lock<std::mutex>& lock, T item) {
    if (closed_) {
      return false;
    }
    items_.push_back(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  template <typename Pred>
  bool evict_locked(Pred& evictable) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (evictable(*it)) {
        items_.erase(it);
        return true;
      }
    }
    return false;
  }

  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> items_;
  bool closed_ = false;
};

}  // namespace gw::core
