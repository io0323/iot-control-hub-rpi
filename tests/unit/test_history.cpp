// 履歴（Phase 5）：HistoryStore（移行・書き込み・保持期間の削除・起動時の復元・DIAG の読み出し）、
// RetryBuffer（1万件の上限と捨てる順。open-questions #41）、HistoryBatcher（100件・200ms の一括、失敗時の
// 保持と再試行、欠けた期間の記録）、next_daily（3時の削除）
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <thread>

#include <unistd.h>

#include "adapters/history/HistoryStore.h"
#include "adapters/history/HistoryWorker.h"
#include "core/history/RetryBuffer.h"

namespace h = gw::core::history;
using gw::adapters::HistoryStore;
using gw::core::Millis;
using gw::core::SteadyTime;

namespace {

class TempDb {
 public:
  TempDb() {
    dir_ = std::filesystem::temp_directory_path() /
           ("gw-history-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  TempDb(TempDb&&) = delete;
  TempDb& operator=(TempDb&&) = delete;
  [[nodiscard]] std::filesystem::path path() const { return dir_ / "history.db"; }

 private:
  static inline int counter_ = 0;
  std::filesystem::path dir_;
};

void raw_exec(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
  const std::string msg = err != nullptr ? err : "";
  sqlite3_free(err);
  sqlite3_close(db);
  ASSERT_EQ(rc, SQLITE_OK) << msg;
}

std::string one(HistoryStore& store, const std::string& sql) {
  const auto rows = store.query(sql);
  return rows.empty() || rows[0].empty() ? "" : rows[0][0];
}

h::HistoryRecord event(const std::string& id, const std::string& severity, std::int64_t ts = 1) {
  return h::record(h::EventRow{id, "gw-01", "X", severity, "{}", ts});
}

h::HistoryRecord of_tier(h::Tier t, int n) {
  h::HistoryRecord r = event("t" + std::to_string(n), "info");
  r.tier = t;
  return r;
}

class FakeSink final : public gw::adapters::IHistorySink {
 public:
  bool fail = false;
  std::vector<std::vector<h::HistoryRecord>> batches;
  void write(const std::vector<h::HistoryRecord>& records) override {
    if (fail) {
      throw std::runtime_error("disk full");
    }
    batches.push_back(records);
  }
  std::uint64_t purge(std::int64_t /*cutoff_ms*/) override { return 0; }
  [[nodiscard]] std::size_t written() const {
    std::size_t n = 0;
    for (const auto& b : batches) {
      n += b.size();
    }
    return n;
  }
};

}  // namespace

// ------------------------------------------------------------ HistoryStore

TEST(HistoryStore, MigratesAnEmptyDatabaseToTheLatestVersion) {
  TempDb tmp;
  HistoryStore store(tmp.path());
  EXPECT_EQ(store.schema_version(), HistoryStore::kSchemaVersion);
  EXPECT_EQ(one(store, "PRAGMA journal_mode"), "wal");
  EXPECT_EQ(one(store, "PRAGMA synchronous"), "1");  // NORMAL
  for (const auto* table : {"devices", "device_status_log", "events", "rule_firings", "commands",
                            "command_transitions", "requests", "system_mode_log", "uart_diag"}) {
    EXPECT_EQ(
        one(store, std::string{"SELECT count(*) FROM sqlite_master WHERE name = '"} + table + "'"),
        "1")
        << table;
  }
}

TEST(HistoryStore, MigratesFromAnOlderVersionAndKeepsRows) {
  TempDb tmp;
  raw_exec(tmp.path(), HistoryStore::migrations()[0] +
                           "PRAGMA user_version=1; INSERT INTO devices(device_id, transport, "
                           "channels, first_seen, last_seen, status) VALUES('mcu-cpp-01', 'mqtt', "
                           "'[]', 1, 2, 'online');");
  HistoryStore store(tmp.path());
  EXPECT_EQ(store.schema_version(), 2);
  EXPECT_EQ(one(store, "SELECT status FROM devices WHERE device_id = 'mcu-cpp-01'"), "online");
  EXPECT_EQ(one(store, "SELECT count(*) FROM uart_diag"), "0");
}

TEST(HistoryStore, RefusesANewerSchemaAndUnopenablePaths) {
  TempDb tmp;
  raw_exec(tmp.path(), "PRAGMA user_version=99;");
  EXPECT_THROW(HistoryStore{tmp.path()}, gw::adapters::HistoryError);
  EXPECT_THROW(HistoryStore{"/dev/null/nope/history.db"}, gw::adapters::HistoryError);
}

TEST(HistoryStore, WritesEveryRowKindAndUpserts) {
  TempDb tmp;
  HistoryStore store(tmp.path());
  store.write({
      event("m1", "warning"),
      event("m1", "warning"),  // 同じ msg_id は1つ
      h::record(
          h::DeviceRow{"mcu-cpp-01", "mqtt", R"([{"channel":"temp_air"}])", "[]", "unseen", 10}),
      h::record(
          h::DeviceRow{"mcu-cpp-01", "mqtt", R"([{"channel":"temp_air"}])", "[]", "online", 20}),
      h::record(h::DeviceStatusRow{"mcu-cpp-01", "unseen", "online", "-", 20}),
      h::record(h::RuleFiringRow{"cool", "idle", "active", "{}", 30}),
      h::record(h::CommandRow{"c1", "mcu-cpp-01", "set_actuator", "{}", "{}", "auto", "ISSUED", 0,
                              40, 40}),
      h::record(h::CommandTransitionRow{"c1", "-", "ISSUED", "", 40}),
      h::record(
          h::CommandRow{"c1", "mcu-cpp-01", "set_actuator", "{}", "{}", "auto", "DONE", 1, 50, 50}),
      h::record(h::RequestRow{"r1", "app-01", "manual", "{}", "accepted", "", "c1", 60}),
      h::record(h::RequestRow{"r1", "app-01", "manual", "{}", "done", "", "c1", 70}),
      h::record(h::ModeRow{"NORMAL", "EMERGENCY", "heat", 80}),
      h::record(h::DiagRow{"mcu-c-01", "00000002", 90, 1234, 3, 2, 1, 0, 120, 0}),
  });
  EXPECT_EQ(one(store, "SELECT count(*) FROM events"), "1");
  EXPECT_EQ(one(store, "SELECT status || ',' || first_seen || ',' || last_seen FROM devices"),
            "online,10,20");
  EXPECT_EQ(one(store,
                "SELECT state || ',' || retries || ',' || created_ts || ',' || updated_ts "
                "FROM commands"),
            "DONE,1,40,50");
  EXPECT_EQ(one(store, "SELECT result || ',' || ts FROM requests"), "done,60");  // ts は最初の受付
  EXPECT_EQ(one(store, "SELECT to_mode FROM system_mode_log"), "EMERGENCY");
  EXPECT_EQ(one(store, "SELECT reset_reason || ',' || max_loop_us FROM uart_diag"), "3,120");
  EXPECT_EQ(one(store, "SELECT count(*) FROM rule_firings"), "1");
  EXPECT_EQ(one(store, "SELECT count(*) FROM device_status_log"), "1");
  EXPECT_EQ(one(store, "SELECT count(*) FROM command_transitions"), "1");
}

TEST(HistoryStore, PurgeKeepsDevicesRecentRowsAndTheLastMode) {
  TempDb tmp;
  HistoryStore store(tmp.path());
  store.write({event("old", "info", 100), event("new", "info", 1000),
               h::record(h::ModeRow{"NORMAL", "EMERGENCY", "heat", 100}),
               h::record(h::DeviceRow{"d-01", "mqtt", "[]", "[]", "online", 100}),
               h::record(h::DiagRow{"mcu-c-01", "1", 100, 0, 0, 0, 0, 0, 0, 0})});
  EXPECT_EQ(store.purge(500), 2U);  // events の old と uart_diag
  EXPECT_EQ(one(store, "SELECT msg_id FROM events"), "new");
  EXPECT_EQ(one(store, "SELECT count(*) FROM system_mode_log"), "1");  // 最後のモードは残す
  EXPECT_EQ(one(store, "SELECT count(*) FROM devices"), "1");
  store.write({h::record(h::ModeRow{"EMERGENCY", "NORMAL", "approved_by_gwctl", 200})});
  EXPECT_EQ(store.purge(500), 1U);  // 古いモードは最後でなくなったので消える
  EXPECT_EQ(one(store, "SELECT to_mode FROM system_mode_log"), "NORMAL");
}

TEST(HistoryStore, RestoreClosesUnfinishedCommandsAndReturnsTheLastHour) {
  TempDb tmp;
  const std::int64_t now = 10'000'000;
  {
    HistoryStore store(tmp.path());
    store.write({
        h::record(h::CommandRow{"sent", "d", "op", "{}", "{}", "auto", "SENT", 0, 1, 1}),
        h::record(h::CommandRow{"acc", "d", "op", "{}", "{}", "auto", "ACCEPTED", 0, 1, 1}),
        h::record(h::CommandRow{"done", "d", "op", "{}", "{}", "auto", "DONE", 0, 1, 1}),
        h::record(
            h::RequestRow{"too-old", "app-01", "manual", "{}", "done", "", "", now - 3'600'000}),
        h::record(
            h::RequestRow{"in", "app-01", "manual", "{}", "accepted", "", "acc", now - 3'599'999}),
        h::record(h::ModeRow{"NORMAL", "EMERGENCY", "heat", 5}),
        h::record(h::DeviceRow{"mcu-cpp-01", "mqtt", "[]", "[]", "online", 5}),
    });
  }
  HistoryStore store(tmp.path());  // 再起動
  const auto r = store.restore(now);
  EXPECT_EQ(r.commands_closed, 2U);
  EXPECT_EQ(one(store,
                "SELECT group_concat(cmd_id) FROM (SELECT cmd_id FROM commands WHERE state "
                "= 'FAILED' ORDER BY cmd_id)"),
            "acc,sent");
  EXPECT_EQ(one(store,
                "SELECT count(*) FROM command_transitions WHERE detail = 'restart' AND "
                "to_state = 'FAILED'"),
            "2");
  ASSERT_EQ(r.requests.size(), 1U);  // ちょうど1時間前は忘れている
  EXPECT_EQ(r.requests[0].request_id, "in");
  EXPECT_EQ(r.requests[0].result, "accepted");
  ASSERT_TRUE(r.last_mode);
  EXPECT_EQ(r.last_mode->to_mode, "EMERGENCY");
  EXPECT_EQ(r.last_mode->reason, "heat");
  ASSERT_EQ(r.devices.size(), 1U);
  EXPECT_EQ(r.devices[0].device_id, "mcu-cpp-01");
  EXPECT_EQ(store.restore(now).commands_closed, 0U);
}

TEST(HistoryStore, ReadDiagsWithAReadOnlyConnection) {
  TempDb tmp;
  HistoryStore store(tmp.path());
  store.write({h::record(h::DiagRow{"mcu-c-01", "1", 100, 10, 1, 0, 0, 0, 0, 0}),
               h::record(h::DiagRow{"mcu-c-01", "1", 200, 20, 3, 0, 0, 0, 0, 0}),
               h::record(h::DiagRow{"other-01", "1", 300, 30, 0, 0, 0, 0, 0, 0})});
  const auto rows = HistoryStore::read_diags(tmp.path(), "mcu-c-01", 150, 5);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].uptime_ms, 20);
  EXPECT_EQ(rows[0].reset_reason, 3);
  EXPECT_EQ(HistoryStore::read_diags(tmp.path(), "mcu-c-01", 0, 5).size(), 2U);
  EXPECT_THROW((void)HistoryStore::read_diags("/no/such.db", "x", 0, 1),
               gw::adapters::HistoryError);
}

// ------------------------------------------------------------ 区分と RetryBuffer

TEST(HistoryRecord, TiersFollowTheDropOrder) {
  EXPECT_EQ(event("a", "info").tier, h::Tier::kInfoEvent);
  EXPECT_EQ(event("a", "warning").tier, h::Tier::kWarningEvent);
  EXPECT_EQ(event("a", "critical").tier, h::Tier::kCriticalEvent);
  EXPECT_EQ(h::record(h::RuleFiringRow{}).tier, h::Tier::kRuleFiring);
  EXPECT_EQ(h::record(h::CommandTransitionRow{"c", "SENT", "ACCEPTED", "", 0}).tier,
            h::Tier::kCommandTransition);
  EXPECT_EQ(h::record(h::CommandTransitionRow{"c", "SENT", "TIMED_OUT", "", 0}).tier,
            h::Tier::kFinal);
  EXPECT_EQ(h::record(h::CommandRow{"c", "d", "o", "", "", "", "SENT", 0, 0, 0}).tier,
            h::Tier::kCommandTransition);
  EXPECT_EQ(h::record(h::CommandRow{"c", "d", "o", "", "", "", "REJECTED", 0, 0, 0}).tier,
            h::Tier::kFinal);
  EXPECT_EQ(h::record(h::DiagRow{}).tier, h::Tier::kDiag);
  EXPECT_EQ(h::record(h::DeviceStatusRow{}).tier, h::Tier::kDeviceStatus);
  EXPECT_EQ(h::record(h::DeviceRow{}).tier, h::Tier::kDeviceStatus);
  EXPECT_EQ(h::record(h::RequestRow{"r", "", "", "", "accepted", "", "", 0}).tier,
            h::Tier::kRequestProgress);
  EXPECT_EQ(h::record(h::RequestRow{"r", "", "", "", "failed", "", "", 0}).tier, h::Tier::kFinal);
  EXPECT_EQ(h::record(h::ModeRow{}).tier, h::Tier::kModeChange);
}

TEST(RetryBuffer, DropsInTheDecidedOrderAndNeverExceedsTheCap) {
  // 区分の大きい順（最終状態から）に1件ずつ入れ、そのあと最終状態を入れ続ける
  h::RetryBuffer buf(h::kTierCount);
  for (int t = h::kTierCount - 1; t >= 0; --t) {
    (void)buf.push(of_tier(static_cast<h::Tier>(t), t));
  }
  ASSERT_EQ(buf.size(), static_cast<std::size_t>(h::kTierCount));
  std::vector<h::Tier> dropped;
  for (int i = 0; i < h::kTierCount - 1; ++i) {
    std::array<std::uint64_t, h::kTierCount> before{};
    for (int t = 0; t < h::kTierCount; ++t) {
      before.at(t) = buf.dropped(static_cast<h::Tier>(t));
    }
    EXPECT_TRUE(buf.push(of_tier(h::Tier::kFinal, 100 + i)));
    for (int t = 0; t < h::kTierCount; ++t) {
      if (buf.dropped(static_cast<h::Tier>(t)) != before.at(t)) {
        dropped.push_back(static_cast<h::Tier>(t));
      }
    }
    EXPECT_EQ(buf.size(), static_cast<std::size_t>(h::kTierCount));
  }
  // info のイベント → ルールの発火 → 指令の途中の遷移 → DIAG → デバイスの状態 → warning のイベント
  // → 要求の途中 → critical のイベント → モードの変化（#41）
  EXPECT_EQ(dropped,
            (std::vector<h::Tier>{
                h::Tier::kInfoEvent, h::Tier::kRuleFiring, h::Tier::kCommandTransition,
                h::Tier::kDiag, h::Tier::kDeviceStatus, h::Tier::kWarningEvent,
                h::Tier::kRequestProgress, h::Tier::kCriticalEvent, h::Tier::kModeChange}));
  // 最終状態だけになったら、最終状態の古いものから捨てる（上限は守る）
  EXPECT_TRUE(buf.push(of_tier(h::Tier::kFinal, 999)));
  EXPECT_EQ(buf.dropped(h::Tier::kFinal), 1U);
  const auto all = buf.take(buf.size());
  EXPECT_EQ(std::get<h::EventRow>(all.front().row).msg_id, "t100");  // 最初の最終状態（t9）が消えた
  EXPECT_EQ(std::get<h::EventRow>(all.back().row).msg_id, "t999");
}

TEST(RetryBuffer, TenThousandIsTheBoundary) {
  h::RetryBuffer buf;
  EXPECT_EQ(buf.capacity(), 10'000U);
  for (int i = 0; i < 10'000; ++i) {
    EXPECT_FALSE(buf.push(event("e" + std::to_string(i), "info")));
  }
  EXPECT_EQ(buf.dropped(), 0U);
  EXPECT_TRUE(buf.push(event("e10000", "info")));
  EXPECT_EQ(buf.size(), 10'000U);
  EXPECT_EQ(buf.dropped(), 1U);
  EXPECT_EQ(std::get<h::EventRow>(buf.take(1)[0].row).msg_id, "e1");  // いちばん古い e0 を捨てた
  // 新しい1件のほうが区分が小さければ、それを捨てる
  h::RetryBuffer small(1);
  (void)small.push(of_tier(h::Tier::kFinal, 1));
  EXPECT_TRUE(small.push(of_tier(h::Tier::kInfoEvent, 2)));
  EXPECT_EQ(std::get<h::EventRow>(small.take(1)[0].row).msg_id, "t1");
  buf.reset_dropped();
  EXPECT_EQ(buf.dropped(), 0U);
}

TEST(RetryBuffer, RestorePutsBackInOrderWithinTheCap) {
  h::RetryBuffer buf(3);
  (void)buf.push(event("a", "info"));
  (void)buf.push(event("b", "critical"));
  auto taken = buf.take(2);
  (void)buf.push(event("c", "warning"));
  (void)buf.push(event("d", "info"));
  buf.restore(std::move(taken));  // a b c d → 上限3：info の古い a を捨てる
  const auto all = buf.take(10);
  ASSERT_EQ(all.size(), 3U);
  EXPECT_EQ(std::get<h::EventRow>(all[0].row).msg_id, "b");
  EXPECT_EQ(std::get<h::EventRow>(all[1].row).msg_id, "c");
  EXPECT_EQ(std::get<h::EventRow>(all[2].row).msg_id, "d");
}

// ------------------------------------------------------------ HistoryBatcher

TEST(HistoryBatcher, WritesAtOneHundredRecords) {
  FakeSink sink;
  gw::adapters::HistoryBatcher b(sink);
  const SteadyTime t0{};
  for (int i = 0; i < 99; ++i) {
    b.add(event("e" + std::to_string(i), "info"), t0);
    b.poll(t0, 0);
  }
  EXPECT_TRUE(sink.batches.empty());
  b.add(event("e99", "info"), t0);
  b.poll(t0, 0);
  ASSERT_EQ(sink.batches.size(), 1U);
  EXPECT_EQ(sink.batches[0].size(), 100U);
  EXPECT_EQ(b.next_deadline(), std::nullopt);
}

TEST(HistoryBatcher, WritesTwoHundredMillisecondsAfterTheFirstRecord) {
  FakeSink sink;
  gw::adapters::HistoryBatcher b(sink);
  const SteadyTime t0{};
  b.add(event("a", "info"), t0);
  b.add(event("b", "info"), t0 + Millis{150});
  EXPECT_EQ(b.next_deadline(), t0 + Millis{200});
  b.poll(t0 + Millis{199}, 0);
  EXPECT_TRUE(sink.batches.empty());
  b.poll(t0 + Millis{200}, 0);
  ASSERT_EQ(sink.batches.size(), 1U);
  EXPECT_EQ(sink.batches[0].size(), 2U);
  // 次の1件からまた数える
  b.add(event("c", "info"), t0 + Millis{1000});
  b.poll(t0 + Millis{1199}, 0);
  EXPECT_EQ(sink.batches.size(), 1U);
  b.poll(t0 + Millis{1200}, 0);
  EXPECT_EQ(sink.batches.size(), 2U);
}

TEST(HistoryBatcher, KeepsRecordsOnFailureRetriesAndRecordsTheGap) {
  FakeSink sink;
  gw::adapters::HistoryBatcher b(sink, 5, "EMERGENCY");
  const SteadyTime t0{};
  sink.fail = true;
  for (int i = 0; i < 3; ++i) {
    b.add(event("e" + std::to_string(i), "info"), t0);
  }
  b.poll(t0 + Millis{200}, 1000);
  EXPECT_TRUE(b.failing());
  EXPECT_EQ(b.stats().write_failures, 1U);
  EXPECT_EQ(b.stats().buffered, 3U);
  EXPECT_EQ(b.next_deadline(), t0 + Millis{1200});  // 1秒後に再試行
  // 保持の上限（5）を超えた分は info から捨てる
  for (int i = 3; i < 8; ++i) {
    b.add(event("e" + std::to_string(i), "info"), t0 + Millis{300});
  }
  b.add(h::record(h::RequestRow{"r", "app-01", "manual", "{}", "done", "", "c", 0}),
        t0 + Millis{300});
  EXPECT_EQ(b.take_new_drops(), 4U);
  EXPECT_EQ(b.take_new_drops(), 0U);
  b.poll(t0 + Millis{1199}, 1000);
  EXPECT_TRUE(sink.batches.empty());
  sink.fail = false;
  b.poll(t0 + Millis{1200}, 2000);
  EXPECT_FALSE(b.failing());
  ASSERT_EQ(sink.batches.size(), 2U);
  EXPECT_EQ(sink.batches[0].size(), 5U);
  // 復旧後に「この期間の履歴は欠けている」ことを system_mode_log に書く
  ASSERT_EQ(sink.batches[1].size(), 1U);
  const auto& gap = std::get<h::ModeRow>(sink.batches[1][0].row);
  EXPECT_EQ(gap.from_mode, "EMERGENCY");
  EXPECT_EQ(gap.to_mode, "EMERGENCY");
  EXPECT_EQ(gap.reason,
            "history_gap from=1970-01-01T00:00:01.000Z to=1970-01-01T00:00:02.000Z dropped=4");
  EXPECT_EQ(b.stats().gaps, 1U);
  EXPECT_EQ(b.stats().written, 5U);
}

TEST(HistoryBatcher, NoGapRowWhenNothingWasLostAndFlushWritesTheRest) {
  FakeSink sink;
  gw::adapters::HistoryBatcher b(sink);
  sink.fail = true;
  b.add(event("a", "info"), SteadyTime{});
  b.poll(SteadyTime{} + Millis{200}, 0);
  sink.fail = false;
  b.poll(SteadyTime{} + Millis{1200}, 0);
  EXPECT_EQ(sink.batches.size(), 1U);  // 欠けていないので gap は書かない
  b.add(h::record(h::ModeRow{"NORMAL", "ALERT", "r", 5}), SteadyTime{} + Millis{2000});
  b.flush(SteadyTime{} + Millis{2000}, 0);
  EXPECT_EQ(sink.written(), 2U);
}

TEST(HistoryWorker, NextDailyPurgeAtThreeLocal) {
  const auto at = [](const char* ts) { return *gw::core::parse_ts(ts); };
  constexpr int kJst = 9 * 60;
  // 2026-11-26 12:00 JST（03:00 UTC）→ 翌日 03:00 JST（2026-11-26 18:00 UTC）
  EXPECT_EQ(gw::core::format_ts(gw::adapters::next_daily(at("2026-11-26T03:00:00.000Z"), 3, kJst)),
            "2026-11-26T18:00:00.000Z");
  // 2026-11-27 02:59 JST → 同じ日の 03:00 JST
  EXPECT_EQ(gw::core::format_ts(gw::adapters::next_daily(at("2026-11-26T17:59:00.000Z"), 3, kJst)),
            "2026-11-26T18:00:00.000Z");
  // ちょうど 03:00 なら翌日
  EXPECT_EQ(gw::core::format_ts(gw::adapters::next_daily(at("2026-11-26T18:00:00.000Z"), 3, kJst)),
            "2026-11-27T18:00:00.000Z");
  EXPECT_EQ(gw::core::format_ts(gw::adapters::next_daily(at("2026-11-26T03:00:00.000Z"), 3, 0)),
            "2026-11-27T03:00:00.000Z");
}

TEST(HistoryWorker, WritesEverythingAndStopsWhenTheQueueCloses) {
  TempDb tmp;
  HistoryStore store(tmp.path());
  gw::core::BoundedQueue<h::HistoryRecord> queue(16);
  gw::core::SystemClock clock;
  std::uint64_t dropped = 0;
  gw::adapters::HistoryWorker worker(
      store, queue, clock, [](gw::core::SystemTime) { return 0; },
      [&](std::uint64_t n) { dropped += n; }, "NORMAL");
  std::thread t([&] { worker.run(); });
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue.push(event("w" + std::to_string(i), "info")));
  }
  queue.close();
  t.join();
  EXPECT_EQ(one(store, "SELECT count(*) FROM events"), "5");
  EXPECT_EQ(worker.stats().written, 5U);
  EXPECT_EQ(dropped, 0U);
}
