#include "core/common/Clock.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace gw::core {

namespace {

constexpr int kTsLength = 24;  // "YYYY-MM-DDTHH:MM:SS.mmmZ"

bool parse_int(std::string_view text, std::size_t pos, std::size_t len, int& out) {
  if (pos + len > text.size()) {
    return false;
  }
  const char* first = text.data() + pos;
  const char* last = first + len;
  for (const char* p = first; p != last; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
  }
  return std::from_chars(first, last, out).ec == std::errc{};
}

}  // namespace

SteadyTime SystemClock::steady_now() const {
  return std::chrono::steady_clock::now();
}

SystemTime SystemClock::system_now() const {
  return std::chrono::floor<Millis>(std::chrono::system_clock::now());
}

VirtualClock::VirtualClock(SystemTime start) : system_(start) {
}

SteadyTime VirtualClock::steady_now() const {
  const std::lock_guard lock(mutex_);
  return steady_;
}

SystemTime VirtualClock::system_now() const {
  const std::lock_guard lock(mutex_);
  return system_;
}

void VirtualClock::advance(Millis delta) {
  const std::lock_guard lock(mutex_);
  steady_ += delta;
  system_ += delta;
}

void VirtualClock::set_system(SystemTime at) {
  const std::lock_guard lock(mutex_);
  system_ = at;
}

std::string format_ts(SystemTime at) {
  using namespace std::chrono;
  const auto day = floor<days>(at);
  const year_month_day ymd{day};
  const hh_mm_ss hms{at - day};
  std::array<char, kTsLength + 1> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ",
                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()),
                static_cast<int>(hms.subseconds().count()));
  return {buffer.data(), static_cast<std::size_t>(kTsLength)};
}

std::optional<SystemTime> parse_ts(std::string_view text) {
  using namespace std::chrono;
  if (text.size() != kTsLength || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
    return std::nullopt;
  }
  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int s = 0;
  int ms = 0;
  if (!parse_int(text, 0, 4, y) || !parse_int(text, 5, 2, mo) || !parse_int(text, 8, 2, d) ||
      !parse_int(text, 11, 2, h) || !parse_int(text, 14, 2, mi) || !parse_int(text, 17, 2, s) ||
      !parse_int(text, 20, 3, ms)) {
    return std::nullopt;
  }
  const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)},
                           day{static_cast<unsigned>(d)}};
  if (!ymd.ok() || h > 23 || mi > 59 || s > 59) {
    return std::nullopt;
  }
  return SystemTime{sys_days{ymd}} + hours{h} + minutes{mi} + seconds{s} + milliseconds{ms};
}

std::int64_t unix_millis(SystemTime at) {
  return at.time_since_epoch().count();
}

}  // namespace gw::core
