// データ品質：iotspec（quality.py・dedup.py）で作ったテストベクタと同じ結果になる
// （tests/data/quality_vectors.json は tools/gen_quality_vectors.py が作る）
#include <gtest/gtest.h>

#include <fstream>

#include <nlohmann/json.hpp>

#include "core/quality/Quality.h"

namespace q = gw::core::quality;
using gw::core::parse_ts;
using nlohmann::json;

namespace {

const json& vectors() {
  static const json v = [] {
    std::ifstream in(std::string{GW_TEST_DATA_DIR} + "/quality_vectors.json");
    return json::parse(in);
  }();
  return v;
}

}  // namespace

TEST(QualityVectors, StaleMatchesIotspec) {
  ASSERT_FALSE(vectors()["stale"].empty());
  for (const auto& c : vectors()["stale"]) {
    EXPECT_EQ(q::is_stale(std::chrono::milliseconds{c["elapsed_ms"].get<std::int64_t>()},
                          c["period_ms"].get<std::int64_t>()),
              c["stale"].get<bool>())
        << c.dump();
  }
}

TEST(QualityVectors, ClockSkewMatchesIotspec) {
  for (const auto& c : vectors()["clock_skew"]) {
    EXPECT_EQ(q::is_clock_skew(*parse_ts(c["ts"].get<std::string>()),
                               *parse_ts(c["received_at"].get<std::string>())),
              c["skew"].get<bool>())
        << c.dump();
  }
}

TEST(QualityVectors, LateMatchesIotspec) {
  for (const auto& c : vectors()["late"]) {
    EXPECT_EQ(q::is_late(*parse_ts(c["ts"].get<std::string>()),
                         *parse_ts(c["received_at"].get<std::string>()),
                         c["period_ms"].get<std::int64_t>()),
              c["late"].get<bool>())
        << c.dump();
  }
}

TEST(QualityVectors, DedupMatchesIotspec) {
  for (const auto& set : vectors()["dedup"]) {
    q::DedupWindow window(set["capacity"].get<std::size_t>());
    int index = 0;
    for (const auto& op : set["ops"]) {
      EXPECT_EQ(window.is_duplicate(op["device_id"], op["boot_id"], op["kind"],
                                    op["seq"].get<std::uint32_t>()),
                op["duplicate"].get<bool>())
          << "op " << index << ": " << op.dump();
      ++index;
    }
  }
}

TEST(QualityVectors, ExemptMatchesIotspec) {
  for (const auto& c : vectors()["exempt"]) {
    EXPECT_EQ(q::exempt_from_seq_checks(gw::core::Json::parse(c["message"].dump())),
              c["exempt"].get<bool>())
        << c.dump();
  }
}

TEST(Dedup, BootIdChangeResetsWindowAndCapacityEvictsOldest) {
  q::DedupWindow window(1024);
  EXPECT_FALSE(window.is_duplicate("d", "00000001", "telemetry", 5));
  EXPECT_TRUE(window.is_duplicate("d", "00000001", "telemetry", 5));
  EXPECT_FALSE(window.is_duplicate("d", "00000002", "telemetry", 5));  // 新しい起動
  EXPECT_FALSE(window.is_duplicate("d", "00000002", "event", 5));      // kind ごと
  for (std::uint32_t s = 6; s < 6 + 1024; ++s) {
    (void)window.is_duplicate("d", "00000002", "telemetry", s);
  }
  EXPECT_EQ(window.remembered("d", "telemetry"), 1024U);
  EXPECT_FALSE(
      window.is_duplicate("d", "00000002", "telemetry", 5));  // 1,024個より前は覚えていない
  EXPECT_THROW(q::DedupWindow(0), std::invalid_argument);
}

TEST(GapCounter, CountsJumpsWithinBootOnly) {
  q::GapCounter gaps;
  EXPECT_EQ(gaps.observe("d", "b1", "telemetry", 0), 0U);
  EXPECT_EQ(gaps.observe("d", "b1", "telemetry", 1), 0U);
  EXPECT_EQ(gaps.observe("d", "b1", "telemetry", 5), 3U);
  EXPECT_EQ(gaps.observe("d", "b1", "event", 9), 0U);      // kind ごとに別
  EXPECT_EQ(gaps.observe("d", "b1", "telemetry", 4), 0U);  // 戻った seq は欠損に数えない
  EXPECT_EQ(gaps.observe("d", "b2", "telemetry", 100), 0U);  // 新しい起動
  EXPECT_EQ(gaps.observe("d", "b2", "telemetry", 102), 1U);
}

TEST(ClockSkewLimiter, OncePerHourPerDevice) {
  q::ClockSkewLimiter limiter;
  const gw::core::SteadyTime t0{};
  EXPECT_TRUE(limiter.allow("a", t0));
  EXPECT_FALSE(limiter.allow("a", t0 + std::chrono::minutes{59}));
  EXPECT_TRUE(limiter.allow("b", t0 + std::chrono::minutes{1}));
  EXPECT_TRUE(limiter.allow("a", t0 + std::chrono::minutes{60}));
}
