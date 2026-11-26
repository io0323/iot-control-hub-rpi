// 送信制限（詳細設計書 第6章「送信制限」、open-questions #5）。
// 対応する Python：iotspec.sim.ratelimit.RateLimiter
// 発信元・kind ごとに、平均で毎秒 rate 件、瞬間的には毎秒 burst 件まで。
// - 平均：トークンバケット（補充 rate 件/秒、容量 burst 件。最初は満杯）
// - 瞬間：直近1秒（スライディングウィンドウ）の送信件数が burst 未満のときだけ送る
// トークンバケットだけだと、満杯から burst 件送った直後の補充分で
// 1秒あたり burst を超えうるので両方で守る。
// 時刻は呼び出し側が渡す（仮想時計で試せる）。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>

#include "core/common/Clock.h"

namespace gw::core {

class TokenBucket {
 public:
  static constexpr std::chrono::seconds kWindow{1};

  TokenBucket(double burst, double rate_per_s, SteadyTime now)
      : burst_(burst), rate_(rate_per_s), tokens_(burst), last_(now) {}

  // 1件送ってよければ記録して true
  bool try_take(SteadyTime now) {
    refill(now);
    if (tokens_ < 1.0 || static_cast<double>(sent_.size()) >= burst_) {
      return false;
    }
    tokens_ -= 1.0;
    sent_.push_back(now);
    return true;
  }

  // 次に送れるようになるまでの時間（いま送れれば0）
  [[nodiscard]] std::chrono::nanoseconds wait_time(SteadyTime now) {
    refill(now);
    std::chrono::nanoseconds wait{0};
    if (tokens_ < 1.0) {
      wait = std::chrono::ceil<std::chrono::nanoseconds>(
          std::chrono::duration<double>((1.0 - tokens_) / rate_));
    }
    if (static_cast<double>(sent_.size()) >= burst_) {
      wait = std::max<std::chrono::nanoseconds>(wait, sent_.front() + kWindow - now);
    }
    return wait;
  }

  [[nodiscard]] double tokens(SteadyTime now) {
    refill(now);
    return tokens_;
  }

 private:
  void refill(SteadyTime now) {
    if (now > last_) {
      const std::chrono::duration<double> elapsed = now - last_;
      tokens_ = std::min(burst_, tokens_ + elapsed.count() * rate_);
      last_ = now;
    }
    while (!sent_.empty() && now - sent_.front() >= kWindow) {
      sent_.pop_front();
    }
  }

  double burst_;
  double rate_;
  double tokens_;
  SteadyTime last_;
  std::deque<SteadyTime> sent_;
};

}  // namespace gw::core
