// Core の履歴・起動時の復元・gwctl の管理要求（Phase 5）を仮想時計で確かめる。
// 履歴の行（イベント・デバイスの状態・ルールの発火・指令と遷移・要求・モード・DIAG）、DATA_DROPPED、
// 復元（1時間の要求、accepted のままの要求、EMERGENCY の引き継ぎ、台帳）、管理要求
// （devices・rules・stats・approve・cmd・diag）
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "adapters/config/RulesConfig.h"
#include "adapters/mqtt/SchemaValidator.h"
#include "core/Core.h"
#include "core/common/Clock.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"

namespace c = gw::core::command;
namespace h = gw::core::history;
namespace u = gw::core::uart;
using gw::core::Json;
using gw::core::Millis;

namespace {

constexpr auto kCpp = "mcu-cpp-01";
constexpr auto kMcuC = "mcu-c-01";

std::vector<u::Profile> devices() {
  return {u::load_profile(GW_PROFILES_DIR, kMcuC), u::load_profile(GW_PROFILES_DIR, kCpp)};
}

Json sample(const std::string& name) {
  std::ifstream in(std::string{GW_IOTSPEC_DIR} + "/samples/" + name);
  std::stringstream text;
  text << in.rdbuf();
  return Json::parse(text.str());
}

constexpr auto kRules = R"(
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 30s, hold: true}
  - id: heat
    priority: safety
    when: {device: mcu-cpp-01, channel: humidity, op: ">=", value: 90.0}
    then:
      - {action: set_system_mode, mode: EMERGENCY}
)";

class CoreHistoryTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-27T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  std::unique_ptr<gw::core::Core> core;
  gw::adapters::SchemaValidator validator{std::string{GW_IOTSPEC_DIR} + "/schemas"};
  std::uint32_t cpp_seq_ = 0;
  std::uint32_t app_seq_ = 0;
  std::uint16_t uart_seq_ = 0;
  std::vector<h::HistoryRecord> history;
  std::vector<gw::core::AdminReply> replies;

  void boot(const std::optional<gw::core::CoreRestore>& restore = std::nullopt) {
    c::RequestRules allow;
    allow.allow.push_back({kCpp, "set_actuator", std::nullopt});
    core = std::make_unique<gw::core::Core>(
        gw::core::CoreConfig{"home", "gw-01", u::load_profile(GW_PROFILES_DIR, "gw-01"),
                             u::load_profile(GW_PROFILES_DIR, kMcuC), "0.1.0", "0badcafe",
                             devices(), allow,
                             gw::adapters::parse_rules_config(kRules, "rules", devices())},
        clock, ids);
    if (restore) {
      core->restore(*restore);
    }
    (void)take(core->start());
  }
  std::vector<Json> take(const gw::core::Outputs& out) {
    std::vector<Json> result;
    for (const auto& m : out.mqtt) {
      const auto v = validator.validate(m.payload);
      EXPECT_TRUE(v.ok) << m.topic << " " << v.reason << " " << v.detail;
      auto j = Json::parse(m.payload);
      j["topic"] = m.topic;
      result.push_back(std::move(j));
    }
    history.insert(history.end(), out.history.begin(), out.history.end());
    replies.insert(replies.end(), out.admin.begin(), out.admin.end());
    return result;
  }
  Json envelope(const std::string& schema, const std::string& device, std::uint32_t seq,
                Json payload) {
    return Json{
        {"schema", schema},      {"msg_id", gw::core::SequentialIdGenerator{clock}.next_ulid()},
        {"device_id", device},   {"seq", seq},
        {"boot_id", "0000000a"}, {"ts", gw::core::format_ts(clock.system_now())},
        {"ts_source", "device"}, {"payload", std::move(payload)}};
  }
  std::vector<Json> mqtt(const Json& message, const std::string& kind) {
    return take(core->step(gw::core::MqttReceived{"t", message, kind, clock.system_now(), false}));
  }
  std::vector<Json> cpp_state(const std::string& status) {
    auto payload = sample("valid/state__online.json")["payload"];
    payload["status"] = status;
    return mqtt(envelope("iot.v1.state", kCpp, cpp_seq_++, payload), "state");
  }
  std::vector<Json> reading(const std::string& channel, double v) {
    const std::string unit = channel == "humidity" ? "pct_rh" : "degC";
    return mqtt(
        envelope(
            "iot.v1.telemetry", kCpp, cpp_seq_++,
            Json{{"readings",
                  Json::array({Json{
                      {"channel", channel}, {"value", v}, {"unit", unit}, {"quality", "good"}}})}}),
        "telemetry");
  }
  std::vector<Json> cpp_ack(const std::string& cmd_id) {
    return mqtt(
        envelope(
            "iot.v1.cmd_ack", kCpp, cpp_seq_++,
            Json{{"cmd_id", cmd_id}, {"status", "done"}, {"limited", false}, {"reason", nullptr}}),
        "cmd/ack");
  }
  std::vector<Json> request(Json payload) {
    return mqtt(envelope("iot.v1.request", "app-01", app_seq_++, std::move(payload)), "request");
  }
  std::vector<Json> admin(const std::string& cmd, Json args = Json::object()) {
    static std::uint64_t next = 100;
    return take(core->step(gw::core::AdminRequest{next++, cmd, std::move(args)}));
  }
  gw::core::Outputs rx(const u::Frame& frame) {
    return core->step(gw::core::UartReceived{u::Decoded{frame}, clock.system_now()});
  }
  static std::vector<Json> with(const std::vector<Json>& out, const std::string& suffix) {
    std::vector<Json> r;
    for (const auto& m : out) {
      if (m["topic"].get<std::string>().ends_with(suffix)) {
        r.push_back(m["payload"]);
      }
    }
    return r;
  }
  template <typename T>
  std::vector<T> rows() const {
    std::vector<T> r;
    for (const auto& rec : history) {
      if (const auto* row = std::get_if<T>(&rec.row)) {
        r.push_back(*row);
      }
    }
    return r;
  }
  static std::string rid(int n) {
    auto s = std::to_string(n);
    return "01JD3V0000000000000000" + std::string(4 - s.size(), '0') + s;
  }
};

}  // namespace

TEST_F(CoreHistoryTest, RecordsEventsDevicesRulesCommandsRequestsAndModes) {
  boot();
  (void)cpp_state("online");
  const auto status = rows<h::DeviceStatusRow>();
  ASSERT_EQ(status.size(), 1U);
  EXPECT_EQ(status[0].device_id, kCpp);
  EXPECT_EQ(status[0].from_status, "unseen");
  EXPECT_EQ(status[0].to_status, "online");
  EXPECT_FALSE(rows<h::DeviceRow>().empty());

  auto out = reading("temp_air", 31.0);
  const auto firing = rows<h::RuleFiringRow>();
  ASSERT_EQ(firing.size(), 1U);
  EXPECT_EQ(firing[0].rule_id, "cool");
  EXPECT_EQ(firing[0].state_to, "active");
  EXPECT_EQ(Json::parse(firing[0].trigger)["values"]["mcu-cpp-01/temp_air"], 31.0);
  const auto events = rows<h::EventRow>();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().code, "RULE_FIRED");  // 発行したイベント
  EXPECT_EQ(events.back().device_id, "gw-01");
  const auto cmd_id = with(out, "/cmd").at(0)["cmd_id"].get<std::string>();
  history.clear();
  (void)cpp_ack(cmd_id);
  const auto cmds = rows<h::CommandRow>();
  ASSERT_FALSE(cmds.empty());
  EXPECT_EQ(cmds.back().cmd_id, cmd_id);
  EXPECT_EQ(cmds.back().state, "DONE");
  EXPECT_EQ(Json::parse(cmds.back().origin)["kind"], "rule");
  EXPECT_EQ(history.back().tier, h::Tier::kFinal);
  const auto transitions = rows<h::CommandTransitionRow>();
  EXPECT_EQ(transitions.back().from_state, "SENT");
  EXPECT_EQ(transitions.back().to_state, "DONE");

  // 要求と結果
  history.clear();
  out = request(Json{{"request_id", rid(1)},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "led_grow"}, {"level", 100}}}});
  auto requests = rows<h::RequestRow>();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].result, "accepted");
  EXPECT_EQ(requests[0].source, "app-01");
  EXPECT_EQ(Json::parse(requests[0].body)["args"]["id"], "led_grow");
  (void)cpp_ack(with(out, "/cmd").at(0)["cmd_id"]);
  requests = rows<h::RequestRow>();
  EXPECT_EQ(requests.back().result, "done");

  // 受信したイベント
  history.clear();
  (void)mqtt(envelope("iot.v1.event", kCpp, cpp_seq_++,
                      Json{{"code", "THRESHOLD_EXCEEDED"}, {"severity", "warning"}}),
             "event");
  ASSERT_EQ(rows<h::EventRow>().size(), 1U);
  EXPECT_EQ(rows<h::EventRow>()[0].device_id, kCpp);
  EXPECT_EQ(history[0].tier, h::Tier::kWarningEvent);

  // モードの変化
  (void)reading("humidity", 95.0);
  const auto modes = rows<h::ModeRow>();
  ASSERT_EQ(modes.size(), 1U);
  EXPECT_EQ(modes[0].from_mode, "NORMAL");
  EXPECT_EQ(modes[0].to_mode, "EMERGENCY");
  EXPECT_EQ(modes[0].reason, "heat");
  // telemetry は記録しない（作品04 の責任）
  for (const auto& e : rows<h::EventRow>()) {
    EXPECT_NE(e.code, "");
  }
}

TEST_F(CoreHistoryTest, DiagGoesToHistoryNotMqtt) {
  boot();
  (void)take(rx(u::State{uart_seq_++, 0x0103, 2, 2000}));
  const auto out = take(rx(u::Diag{uart_seq_++, 1234, 3, 2, 1, 4, 120, 0}));
  for (const auto& m : out) {
    EXPECT_FALSE(m["topic"].get<std::string>().ends_with("/diag"));
  }
  const auto diag = rows<h::DiagRow>();
  ASSERT_EQ(diag.size(), 1U);
  EXPECT_EQ(diag[0].device_id, kMcuC);
  EXPECT_EQ(diag[0].boot_id, "00000002");
  EXPECT_EQ(diag[0].uptime_ms, 1234);
  EXPECT_EQ(diag[0].reset_reason, 3);
  EXPECT_EQ(diag[0].max_loop_us, 120);
}

TEST_F(CoreHistoryTest, HistoryDroppedPublishesDataDropped) {
  boot();
  const auto out = take(core->step(gw::core::HistoryDropped{7, "history"}));
  const auto events = with(out, "gw-01/event");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0]["code"], "DATA_DROPPED");
  EXPECT_EQ(events[0]["severity"], "warning");
  EXPECT_EQ(events[0]["detail"], (Json{{"source", "history"}, {"count", 7}}));
}

TEST_F(CoreHistoryTest, RestoreRemembersRequestsForOneHourAndClosesAcceptedOnes) {
  gw::core::CoreRestore r;
  r.requests.push_back({rid(1), "app-01", "manual", "done", "", rid(9), 3'599'000});
  r.requests.push_back({rid(2), "app-01", "manual", "accepted", "", rid(8), 1'000});
  boot(r);
  (void)cpp_state("online");
  auto out = request(Json{{"request_id", rid(1)},
                          {"kind", "manual"},
                          {"target", kCpp},
                          {"op", "set_actuator"},
                          {"args", Json{{"id", "fan_main"}, {"level", 100}}}});
  ASSERT_EQ(with(out, "/request/ack").size(), 1U);
  EXPECT_EQ(with(out, "/request/ack")[0]["status"], "done");  // 再起動の前に終わった要求
  EXPECT_TRUE(with(out, "/cmd").empty());
  out = request(Json{{"request_id", rid(2)},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "fan_main"}, {"level", 100}}}});
  EXPECT_EQ(with(out, "/request/ack")[0]["status"], "failed");  // #42
  EXPECT_EQ(with(out, "/request/ack")[0]["reason"], "internal");
  EXPECT_EQ(core->requests().stats().duplicates, 2U);
  // 1時間の境界：rid(1) は 3599秒前に受けたので、あと1秒で忘れる
  clock.advance(Millis{1000});
  (void)take(core->on_timer(clock.steady_now()));
  EXPECT_EQ(core->requests().find(rid(1)), nullptr);
  EXPECT_NE(core->requests().find(rid(2)), nullptr);
}

TEST_F(CoreHistoryTest, RestoredEmergencyWaitsForApprovalAndDevicesStartUnknown) {
  gw::core::CoreRestore r;
  r.last_mode = "EMERGENCY";
  r.last_mode_reason = "heat";
  r.devices.push_back(h::DeviceRow{"mcu-cpp-02", "mqtt",
                                   R"([{"channel":"temp_air","unit":"degC","period_ms":1000}])",
                                   "[]", "online", 1});
  boot(r);
  EXPECT_EQ(core->system_mode().mode(), "EMERGENCY");
  auto out = take(core->step(gw::core::MqttConnected{}));
  ASSERT_EQ(with(out, "gw-01/state").size(), 1U);
  EXPECT_EQ(with(out, "gw-01/state")[0]["mode"], "EMERGENCY");
  EXPECT_TRUE(with(out, "gw-01/event").empty());  // 起動時に SYSTEM_MODE_CHANGED は出さない（#44）
  // 台帳：再起動前のデバイスは状態「まだ観測していない」から（gwctl では unknown）
  (void)admin("devices");
  ASSERT_FALSE(replies.empty());
  bool found = false;
  for (const auto& d : replies.back().body["result"]) {
    if (d["device_id"] == "mcu-cpp-02") {
      found = true;
      EXPECT_EQ(d["status"], "unknown");
      EXPECT_EQ(d["channels"][0]["channel"], "temp_air");
    }
  }
  EXPECT_TRUE(found);
  // heat が成立している間は承認できない。回復したら gwctl approve で戻る
  (void)cpp_state("online");
  (void)reading("humidity", 95.0);
  (void)admin("approve");
  EXPECT_EQ(replies.back().body["result"]["status"], "rejected");
  EXPECT_EQ(replies.back().body["result"]["reason"], "unsafe");
  (void)reading("humidity", 50.0);
  out = admin("approve");
  EXPECT_EQ(replies.back().body["result"]["status"], "done");
  EXPECT_EQ(replies.back().body["result"]["mode"], "NORMAL");
  const auto changed = with(out, "gw-01/event");
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0]["detail"],
            (Json{{"from", "EMERGENCY"}, {"to", "NORMAL"}, {"reason", "approved_by_gwctl"}}));
}

TEST_F(CoreHistoryTest, AdminDevicesRulesStatsAndErrors) {
  boot();
  (void)cpp_state("online");
  (void)admin("rules");
  const auto rules = replies.back().body["result"];
  EXPECT_EQ(rules["mode"], "NORMAL");
  ASSERT_EQ(rules["rules"].size(), 2U);
  EXPECT_EQ(rules["rules"][0]["id"], "cool");
  EXPECT_EQ(rules["rules"][0]["state"], "idle");
  (void)admin("stats");
  const auto stats = replies.back().body["result"];
  EXPECT_EQ(stats["quality"].size(), 8U);
  EXPECT_TRUE(stats["commands"].contains("submitted"));
  (void)admin("nope");
  EXPECT_FALSE(replies.back().body["ok"]);
  (void)admin("cmd", Json{{"target", kCpp}, {"op", "reboot"}});
  EXPECT_FALSE(replies.back().body["ok"]);
}

TEST_F(CoreHistoryTest, GwctlManualCommandBeatsTheAutoRule) {
  boot();
  (void)cpp_state("online");
  auto out = reading("temp_air", 31.0);
  ASSERT_EQ(with(out, "/cmd").size(), 1U);
  (void)cpp_ack(with(out, "/cmd")[0]["cmd_id"]);
  const auto before = replies.size();
  out = admin("cmd", Json{{"target", kCpp},
                          {"op", "set_actuator"},
                          {"args", Json{{"id", "fan_main"}, {"level", 300}}},
                          {"lease_s", 60}});
  ASSERT_EQ(with(out, "/cmd").size(), 1U);
  const auto cmd = with(out, "/cmd")[0];
  EXPECT_EQ(cmd["priority"], "manual");
  EXPECT_EQ(cmd["origin"]["kind"], "gwctl");
  EXPECT_TRUE(with(out, "/request/ack").empty());  // gwctl の要求は MQTT に出さない
  EXPECT_EQ(replies.size(), before);               // 最終結果まで待つ
  (void)cpp_ack(cmd["cmd_id"]);
  ASSERT_EQ(replies.size(), before + 1);
  EXPECT_EQ(replies.back().body["result"]["status"], "done");
  EXPECT_EQ(replies.back().body["result"]["cmd_id"], cmd["cmd_id"]);
  EXPECT_EQ(core->arbiter().owner(c::ActuatorKey{kCpp, "fan_main"})->priority,
            c::Priority::kManual);
  // auto のルールの更新は手動の制御を上書きしない
  std::vector<Json> later;
  for (int s = 0; s < 12; ++s) {
    clock.advance(Millis{1000});
    for (auto& m : take(core->on_timer(clock.steady_now()))) {
      later.push_back(std::move(m));
    }
    (void)reading("temp_air", 31.0);
  }
  EXPECT_TRUE(with(later, "/cmd").empty());
  // 許可リストは見ないが、宛先の登録は見る（#46）
  (void)admin("cmd", Json{{"target", "nope-01"}, {"op", "get_status"}});
  EXPECT_EQ(replies.back().body["result"]["status"], "rejected");
  EXPECT_EQ(replies.back().body["result"]["reason"], "not_permitted");
}

TEST_F(CoreHistoryTest, GwctlDiagSendsGetDiagAsAnInternalCommand) {
  boot();
  (void)take(rx(u::State{uart_seq_++, 0x0103, 2, 2000}));
  auto raw = core->step(gw::core::AdminRequest{1, "diag", Json{{"device", kMcuC}}});
  (void)take(raw);
  ASSERT_FALSE(replies.empty());
  EXPECT_EQ(replies.back().body["result"]["status"], "sent");
  std::vector<u::Cmd> frames;
  for (const auto& tx : raw.uart) {
    if (const auto* f = std::get_if<u::Cmd>(&tx.frame)) {
      frames.push_back(*f);
    }
  }
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].op, 0x11);  // GET_DIAG
  // DIAG → ACK(done)。ACK は cmd/ack にしない（#37）
  (void)take(rx(u::Diag{uart_seq_++, 99, 1, 0, 0, 0, 10, 0}));
  const auto ack = take(rx(u::Ack{0, frames[0].cmd_ref, 1, 0, 0, 0}));
  EXPECT_TRUE(with(ack, "/cmd/ack").empty());
  EXPECT_EQ(core->commands().stats().done, 1U);
  EXPECT_EQ(rows<h::DiagRow>().size(), 1U);
  (void)admin("diag", Json{{"device", kCpp}});
  EXPECT_FALSE(replies.back().body["ok"]);
}
