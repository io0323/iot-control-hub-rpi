#include "adapters/history/HistoryStore.h"

#include <sqlite3.h>

#include <array>
#include <utility>

namespace gw::adapters {

namespace {

namespace h = core::history;

// v1：基本設計 第7章の8テーブル
constexpr std::string_view kV1 = R"(
CREATE TABLE devices(device_id TEXT PRIMARY KEY, transport TEXT, channels TEXT,
  first_seen INTEGER, last_seen INTEGER, status TEXT);
CREATE TABLE device_status_log(id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT,
  from_status TEXT, to_status TEXT, reason TEXT, ts INTEGER);
CREATE TABLE events(msg_id TEXT PRIMARY KEY, device_id TEXT, code TEXT, severity TEXT,
  payload TEXT, ts INTEGER);
CREATE TABLE rule_firings(id INTEGER PRIMARY KEY AUTOINCREMENT, rule_id TEXT, state_from TEXT,
  state_to TEXT, trigger TEXT, ts INTEGER);
CREATE TABLE commands(cmd_id TEXT PRIMARY KEY, device_id TEXT, op TEXT, args TEXT, origin TEXT,
  priority TEXT, state TEXT, retries INTEGER, created_ts INTEGER, updated_ts INTEGER);
CREATE TABLE command_transitions(id INTEGER PRIMARY KEY AUTOINCREMENT, cmd_id TEXT,
  from_state TEXT, to_state TEXT, detail TEXT, ts INTEGER);
CREATE TABLE requests(request_id TEXT PRIMARY KEY, source TEXT, kind TEXT, body TEXT,
  result TEXT, reason TEXT, cmd_id TEXT, ts INTEGER);
CREATE TABLE system_mode_log(id INTEGER PRIMARY KEY AUTOINCREMENT, from_mode TEXT, to_mode TEXT,
  reason TEXT, ts INTEGER);
)";

// v2：uart_diag（#1・v0.2.2 #37）、再起動後の台帳のアクチュエーター、保持期間の削除と復元の索引
constexpr std::string_view kV2 = R"(
CREATE TABLE uart_diag(id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT, boot_id TEXT,
  received_ts INTEGER, uptime_ms INTEGER, reset_reason INTEGER, reset_count INTEGER,
  rx_crc_errors INTEGER, rx_frame_errors INTEGER, max_loop_us INTEGER, overruns INTEGER);
ALTER TABLE devices ADD COLUMN actuators TEXT;
CREATE INDEX events_ts ON events(ts);
CREATE INDEX device_status_log_ts ON device_status_log(ts);
CREATE INDEX rule_firings_ts ON rule_firings(ts);
CREATE INDEX command_transitions_ts ON command_transitions(ts);
CREATE INDEX commands_state ON commands(state);
CREATE INDEX requests_ts ON requests(ts);
CREATE INDEX system_mode_log_ts ON system_mode_log(ts);
CREATE INDEX uart_diag_received ON uart_diag(device_id, received_ts);
)";

// 1つの文（prepare・bind・step）
class Statement {
 public:
  Statement(sqlite3* db, std::string_view sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) !=
        SQLITE_OK) {
      throw HistoryError(std::string{"SQL を用意できない: "} + sqlite3_errmsg(db));
    }
  }
  ~Statement() { (void)sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  Statement& bind(int i, const std::string& v) {
    check(sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
    return *this;
  }
  Statement& bind(int i, std::int64_t v) {
    check(sqlite3_bind_int64(stmt_, i, v));
    return *this;
  }
  Statement& bind(int i, int v) { return bind(i, static_cast<std::int64_t>(v)); }
  // 1行進める。行があれば true
  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc != SQLITE_DONE) {
      throw HistoryError(std::string{"SQL を実行できない: "} + sqlite3_errmsg(db_));
    }
    return false;
  }
  void run() {
    (void)step();
    (void)sqlite3_reset(stmt_);
    (void)sqlite3_clear_bindings(stmt_);
  }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = sqlite3_column_text(stmt_, col);
    return p == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(p)};  // NOLINT
  }
  [[nodiscard]] std::int64_t int64(int col) const { return sqlite3_column_int64(stmt_, col); }
  [[nodiscard]] int columns() const { return sqlite3_column_count(stmt_); }

 private:
  void check(int rc) const {
    if (rc != SQLITE_OK) {
      throw HistoryError(std::string{"値を入れられない: "} + sqlite3_errmsg(db_));
    }
  }
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

const std::vector<std::string>& HistoryStore::migrations() {
  static const std::vector<std::string> list{std::string{kV1}, std::string{kV2}};
  return list;
}

HistoryStore::HistoryStore(const std::filesystem::path& path) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    const std::string msg = db_ != nullptr ? sqlite3_errmsg(db_) : "メモリ不足";
    (void)sqlite3_close(db_);
    db_ = nullptr;
    throw HistoryError("履歴DB を開けない（" + path.string() + "）: " + msg);
  }
  (void)sqlite3_busy_timeout(db_, 1000);
  try {
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    migrate();
  } catch (...) {
    (void)sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

HistoryStore::~HistoryStore() {
  (void)sqlite3_close(db_);
}

void HistoryStore::exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string msg = err != nullptr ? err : sqlite3_errmsg(db_);
    sqlite3_free(err);
    throw HistoryError("SQL を実行できない: " + msg);
  }
}

int HistoryStore::schema_version() {
  Statement st(db_, "PRAGMA user_version");
  return st.step() ? static_cast<int>(st.int64(0)) : 0;
}

void HistoryStore::migrate() {
  const auto& list = migrations();
  const int from = schema_version();
  if (from > static_cast<int>(list.size())) {
    throw HistoryError("履歴DB の版（" + std::to_string(from) + "）がこの iot-gw より新しい");
  }
  for (int v = from; v < static_cast<int>(list.size()); ++v) {
    exec("BEGIN");
    try {
      exec(list[static_cast<std::size_t>(v)]);
      exec("PRAGMA user_version=" + std::to_string(v + 1));
      exec("COMMIT");
    } catch (...) {
      (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  }
}

void HistoryStore::write(const std::vector<h::HistoryRecord>& records) {
  if (records.empty()) {
    return;
  }
  exec("BEGIN");
  try {
    Statement event(db_,
                    "INSERT OR IGNORE INTO events(msg_id, device_id, code, severity, payload, ts) "
                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    Statement device(db_,
                     "INSERT INTO devices(device_id, transport, channels, actuators, first_seen, "
                     "last_seen, status) VALUES(?1, ?2, ?3, ?4, ?5, ?5, ?6) "
                     "ON CONFLICT(device_id) DO UPDATE SET transport = excluded.transport, "
                     "channels = excluded.channels, actuators = excluded.actuators, "
                     "last_seen = excluded.last_seen, status = excluded.status");
    Statement status(db_,
                     "INSERT INTO device_status_log(device_id, from_status, to_status, reason, ts) "
                     "VALUES(?1, ?2, ?3, ?4, ?5)");
    Statement firing(db_,
                     "INSERT INTO rule_firings(rule_id, state_from, state_to, trigger, ts) "
                     "VALUES(?1, ?2, ?3, ?4, ?5)");
    Statement command(db_,
                      "INSERT INTO commands(cmd_id, device_id, op, args, origin, priority, state, "
                      "retries, created_ts, updated_ts) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, "
                      "?9, ?10) ON CONFLICT(cmd_id) DO UPDATE SET state = excluded.state, "
                      "retries = excluded.retries, updated_ts = excluded.updated_ts");
    Statement transition(db_,
                         "INSERT INTO command_transitions(cmd_id, from_state, to_state, detail, "
                         "ts) VALUES(?1, ?2, ?3, ?4, ?5)");
    Statement request(db_,
                      "INSERT INTO requests(request_id, source, kind, body, result, reason, "
                      "cmd_id, ts) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                      "ON CONFLICT(request_id) DO UPDATE SET result = excluded.result, "
                      "reason = excluded.reason, cmd_id = excluded.cmd_id");
    Statement mode(db_,
                   "INSERT INTO system_mode_log(from_mode, to_mode, reason, ts) "
                   "VALUES(?1, ?2, ?3, ?4)");
    Statement diag(db_,
                   "INSERT INTO uart_diag(device_id, boot_id, received_ts, uptime_ms, "
                   "reset_reason, reset_count, rx_crc_errors, rx_frame_errors, max_loop_us, "
                   "overruns) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    for (const auto& r : records) {
      std::visit(
          [&](const auto& row) {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::is_same_v<T, h::EventRow>) {
              event.bind(1, row.msg_id).bind(2, row.device_id).bind(3, row.code);
              event.bind(4, row.severity).bind(5, row.payload).bind(6, row.ts_ms).run();
            } else if constexpr (std::is_same_v<T, h::DeviceRow>) {
              device.bind(1, row.device_id).bind(2, row.transport).bind(3, row.channels);
              device.bind(4, row.actuators).bind(5, row.seen_ms).bind(6, row.status).run();
            } else if constexpr (std::is_same_v<T, h::DeviceStatusRow>) {
              status.bind(1, row.device_id).bind(2, row.from_status).bind(3, row.to_status);
              status.bind(4, row.reason).bind(5, row.ts_ms).run();
            } else if constexpr (std::is_same_v<T, h::RuleFiringRow>) {
              firing.bind(1, row.rule_id).bind(2, row.state_from).bind(3, row.state_to);
              firing.bind(4, row.trigger).bind(5, row.ts_ms).run();
            } else if constexpr (std::is_same_v<T, h::CommandRow>) {
              command.bind(1, row.cmd_id).bind(2, row.device_id).bind(3, row.op);
              command.bind(4, row.args).bind(5, row.origin).bind(6, row.priority);
              command.bind(7, row.state).bind(8, row.retries).bind(9, row.created_ms);
              command.bind(10, row.updated_ms).run();
            } else if constexpr (std::is_same_v<T, h::CommandTransitionRow>) {
              transition.bind(1, row.cmd_id).bind(2, row.from_state).bind(3, row.to_state);
              transition.bind(4, row.detail).bind(5, row.ts_ms).run();
            } else if constexpr (std::is_same_v<T, h::RequestRow>) {
              request.bind(1, row.request_id).bind(2, row.source).bind(3, row.kind);
              request.bind(4, row.body).bind(5, row.result).bind(6, row.reason);
              request.bind(7, row.cmd_id).bind(8, row.ts_ms).run();
            } else if constexpr (std::is_same_v<T, h::ModeRow>) {
              mode.bind(1, row.from_mode).bind(2, row.to_mode).bind(3, row.reason);
              mode.bind(4, row.ts_ms).run();
            } else {
              static_assert(std::is_same_v<T, h::DiagRow>);
              diag.bind(1, row.device_id).bind(2, row.boot_id).bind(3, row.received_ms);
              diag.bind(4, row.uptime_ms).bind(5, row.reset_reason).bind(6, row.reset_count);
              diag.bind(7, row.rx_crc_errors).bind(8, row.rx_frame_errors);
              diag.bind(9, row.max_loop_us).bind(10, row.overruns).run();
            }
          },
          r.row);
    }
    exec("COMMIT");
  } catch (...) {
    (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::uint64_t HistoryStore::purge(std::int64_t cutoff_ms) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kTables{{
      {"events", "ts"},
      {"device_status_log", "ts"},
      {"rule_firings", "ts"},
      {"commands", "updated_ts"},
      {"command_transitions", "ts"},
      {"requests", "ts"},
      {"uart_diag", "received_ts"},
  }};
  std::uint64_t deleted = 0;
  exec("BEGIN");
  try {
    for (const auto& [table, column] : kTables) {
      Statement st(db_,
                   "DELETE FROM " + std::string{table} + " WHERE " + std::string{column} + " < ?1");
      st.bind(1, cutoff_ms).run();
      deleted += static_cast<std::uint64_t>(sqlite3_changes(db_));
    }
    // 最後のモードは再起動のために残す（90日より前に EMERGENCY に入ったままでも引き継ぐ）
    Statement st(db_,
                 "DELETE FROM system_mode_log WHERE ts < ?1 AND id < "
                 "(SELECT MAX(id) FROM system_mode_log)");
    st.bind(1, cutoff_ms).run();
    deleted += static_cast<std::uint64_t>(sqlite3_changes(db_));
    exec("COMMIT");
  } catch (...) {
    (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  return deleted;
}

RestoreState HistoryStore::restore(std::int64_t now_ms) {
  RestoreState state;
  // 未完了の指令を FAILED（restart）で閉じる（基本設計 第8章「異常終了」）
  exec("BEGIN");
  try {
    exec(
        "INSERT INTO command_transitions(cmd_id, from_state, to_state, detail, ts) "
        "SELECT cmd_id, state, 'FAILED', 'restart', " +
        std::to_string(now_ms) +
        " FROM commands WHERE state NOT IN ('DONE', 'REJECTED', 'FAILED', 'TIMED_OUT')");
    exec("UPDATE commands SET state = 'FAILED', updated_ts = " + std::to_string(now_ms) +
         " WHERE state NOT IN ('DONE', 'REJECTED', 'FAILED', 'TIMED_OUT')");
    state.commands_closed = static_cast<std::uint64_t>(sqlite3_changes(db_));
    exec("COMMIT");
  } catch (...) {
    (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  {
    Statement st(db_,
                 "SELECT request_id, source, kind, result, reason, cmd_id, ts FROM requests "
                 "WHERE ts > ?1 ORDER BY ts");
    st.bind(1, now_ms - kRequestWindowMs);
    while (st.step()) {
      state.requests.push_back(RestoredRequest{st.text(0), st.text(1), st.text(2), st.text(3),
                                               st.text(4), st.text(5), st.int64(6)});
    }
  }
  {
    Statement st(db_,
                 "SELECT from_mode, to_mode, reason, ts FROM system_mode_log ORDER BY id DESC "
                 "LIMIT 1");
    if (st.step()) {
      state.last_mode = h::ModeRow{st.text(0), st.text(1), st.text(2), st.int64(3)};
    }
  }
  {
    Statement st(db_,
                 "SELECT device_id, transport, channels, actuators, status, last_seen FROM devices "
                 "ORDER BY device_id");
    while (st.step()) {
      state.devices.push_back(
          h::DeviceRow{st.text(0), st.text(1), st.text(2), st.text(3), st.text(4), st.int64(5)});
    }
  }
  return state;
}

std::vector<std::vector<std::string>> HistoryStore::query(const std::string& sql) {
  std::vector<std::vector<std::string>> rows;
  Statement st(db_, sql);
  while (st.step()) {
    std::vector<std::string> row;
    row.reserve(static_cast<std::size_t>(st.columns()));
    for (int c = 0; c < st.columns(); ++c) {
      row.push_back(st.text(c));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<h::DiagRow> HistoryStore::read_diags(const std::filesystem::path& path,
                                                 const std::string& device_id,
                                                 std::int64_t since_ms, int limit) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    const std::string msg = db != nullptr ? sqlite3_errmsg(db) : "メモリ不足";
    (void)sqlite3_close(db);
    throw HistoryError("履歴DB を開けない（" + path.string() + "）: " + msg);
  }
  (void)sqlite3_busy_timeout(db, 1000);
  std::vector<h::DiagRow> rows;
  try {
    Statement st(db,
                 "SELECT device_id, boot_id, received_ts, uptime_ms, reset_reason, reset_count, "
                 "rx_crc_errors, rx_frame_errors, max_loop_us, overruns FROM uart_diag "
                 "WHERE device_id = ?1 AND received_ts >= ?2 ORDER BY received_ts DESC LIMIT ?3");
    st.bind(1, device_id).bind(2, since_ms).bind(3, limit);
    while (st.step()) {
      rows.push_back(h::DiagRow{st.text(0), st.text(1), st.int64(2), st.int64(3),
                                static_cast<int>(st.int64(4)), static_cast<int>(st.int64(5)),
                                static_cast<int>(st.int64(6)), static_cast<int>(st.int64(7)),
                                static_cast<int>(st.int64(8)), static_cast<int>(st.int64(9))});
    }
  } catch (...) {
    (void)sqlite3_close(db);
    throw;
  }
  (void)sqlite3_close(db);
  return rows;
}

}  // namespace gw::adapters
