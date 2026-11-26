// 時計（詳細設計書 第4章「時刻と ID」）。
// steady_now は継続時間・タイムアウト用（単調増加）、system_now はエンベロープの ts（UTC ミリ秒）。
// 対応する Python：iotspec.clock（Clock・VirtualClock・format_ts・parse_ts）
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace gw::core {

using SteadyTime = std::chrono::steady_clock::time_point;
using SystemTime = std::chrono::sys_time<std::chrono::milliseconds>;
using Millis = std::chrono::milliseconds;

class IClock {
 public:
  IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  IClock(IClock&&) = delete;
  IClock& operator=(IClock&&) = delete;
  virtual ~IClock() = default;

  [[nodiscard]] virtual SteadyTime steady_now() const = 0;
  [[nodiscard]] virtual SystemTime system_now() const = 0;
};

class SystemClock final : public IClock {
 public:
  [[nodiscard]] SteadyTime steady_now() const override;
  [[nodiscard]] SystemTime system_now() const override;
};

// 手で進める時計（テスト用）。steady と system を同じだけ進める。
class VirtualClock final : public IClock {
 public:
  explicit VirtualClock(SystemTime start);

  [[nodiscard]] SteadyTime steady_now() const override;
  [[nodiscard]] SystemTime system_now() const override;

  void advance(Millis delta);
  // system の時刻だけを指定の値にする（ゴールデンの received_at を入れる）
  void set_system(SystemTime at);

 private:
  mutable std::mutex mutex_;
  SteadyTime steady_{};
  SystemTime system_;
};

// "2026-11-20T06:00:12.345Z"（UTC、ミリ秒3桁、末尾 Z）
[[nodiscard]] std::string format_ts(SystemTime at);
// format_ts の逆。形式違いは nullopt
[[nodiscard]] std::optional<SystemTime> parse_ts(std::string_view text);
// Unix エポックからのミリ秒
[[nodiscard]] std::int64_t unix_millis(SystemTime at);

}  // namespace gw::core
