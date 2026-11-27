// Core のルールとシステムモード（Phase 4）を仮想時計で確かめる。発行した電文はすべて JSON Schema で検証する。
// 10秒の for・hold と更新・ヒステリシスでの解除と release、stale での更新停止と再開、EMERGENCY と
// schedule の拒否、承認（v0.2.3 #46）、EMERGENCY 中の auto の停止と再開（#34）、ALERT の自動の戻り（#29）、
// 手動 release でルールが黙ること（#26）、上位の制御を更新で上書きしないこと（#33）、reconnect（#32）、
// 再読込（#39）、品質の件数の telemetry（v0.2.3 #45）、時刻帯、quality が good でない値
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
namespace u = gw::core::uart;
using gw::core::Json;
using gw::core::Millis;

namespace {

constexpr auto kCpp = "mcu-cpp-01";
constexpr auto kMcuC = "mcu-c-01";

std::vector<u::Profile> devices() {
  return {u::load_profile(GW_PROFILES_DIR, kMcuC), u::load_profile(GW_PROFILES_DIR, kCpp)};
}

c::RequestRules request_rules() {
  c::RequestRules r;
  r.allow.push_back({kCpp, "set_actuator", std::nullopt});
  r.allow.push_back({kCpp, "release", std::nullopt});
  return r;
}

Json sample(const std::string& name) {
  std::ifstream in(std::string{GW_IOTSPEC_DIR} + "/samples/" + name);
  std::stringstream text;
  text << in.rdbuf();
  return Json::parse(text.str());
}

struct Published {
  std::string topic;
  Json message;
};

constexpr auto kCool = R"(
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    for: 10s
    hysteresis: 2.0
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 30s, hold: true}
)";

constexpr auto kCoolNow = R"(
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    hysteresis: 2.0
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 30s, hold: true}
)";

class CoreRulesTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-26T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  std::unique_ptr<gw::core::Core> core;
  gw::adapters::SchemaValidator validator{std::string{GW_IOTSPEC_DIR} + "/schemas"};
  std::uint32_t app_seq_ = 0;
  std::uint32_t cpp_seq_ = 0;
  int request_n_ = 0;

  void boot(const std::string& rules_yaml, bool cpp_online = true,
            const std::optional<int>& utc_offset_min = std::nullopt) {
    core = std::make_unique<gw::core::Core>(
        gw::core::CoreConfig{"home", "gw-01", u::load_profile(GW_PROFILES_DIR, "gw-01"),
                             u::load_profile(GW_PROFILES_DIR, kMcuC), "0.1.0", "0badcafe",
                             devices(), request_rules(),
                             gw::adapters::parse_rules_config(rules_yaml, "rules", devices())},
        clock, ids);
    if (utc_offset_min) {
      core->set_utc_offset([m = *utc_offset_min](gw::core::SystemTime) { return m; });
    }
    (void)take(core->start());
    (void)take(core->step(gw::core::MqttConnected{}));
    if (cpp_online) {
      cpp_state("online");
    }
  }

  std::vector<Published> take(const gw::core::Outputs& out) {
    std::vector<Published> result;
    for (const auto& m : out.mqtt) {
      const auto v = validator.validate(m.payload);
      EXPECT_TRUE(v.ok) << m.topic << " " << v.reason << " " << v.detail;
      result.push_back(Published{m.topic, Json::parse(m.payload)});
    }
    return result;
  }
  Json envelope(const std::string& schema, const std::string& device, std::uint32_t seq,
                Json payload) {
    return Json{{"schema", schema},      {"msg_id", "01JD3R0A5K7M2N4P6R8T0V2X4Y"},
                {"device_id", device},   {"seq", seq},
                {"boot_id", "0000000a"}, {"ts", gw::core::format_ts(clock.system_now())},
                {"ts_source", "device"}, {"payload", std::move(payload)}};
  }
  gw::core::Outputs mqtt_raw(const Json& message, const std::string& kind) {
    return core->step(gw::core::MqttReceived{"t", message, kind, clock.system_now(), false});
  }
  std::vector<Published> mqtt(const Json& message, const std::string& kind) {
    return take(mqtt_raw(message, kind));
  }
  std::vector<Published> cpp_state(const std::string& status) {
    auto payload = sample("valid/state__online.json")["payload"];
    payload["status"] = status;
    return mqtt(envelope("iot.v1.state", kCpp, cpp_seq_++, payload), "state");
  }
  std::vector<Published> reading(const std::string& channel, double value,
                                 const std::string& quality = "good") {
    const std::string unit = channel == "humidity" ? "pct_rh" : "degC";
    Json payload{
        {"readings",
         Json::array({Json{
             {"channel", channel}, {"value", value}, {"unit", unit}, {"quality", quality}}})}};
    return mqtt(envelope("iot.v1.telemetry", kCpp, cpp_seq_++, payload), "telemetry");
  }
  std::vector<Published> temp(double v) { return reading("temp_air", v); }
  std::vector<Published> advance(Millis d) {
    clock.advance(d);
    return take(core->on_timer(clock.steady_now()));
  }
  // 1秒ごとに値を送りながら seconds 秒進める。出力をまとめて返す
  std::vector<Published> hold_temp(double v, int seconds) {
    std::vector<Published> all;
    for (int i = 0; i < seconds; ++i) {
      for (auto& p : advance(Millis{1000})) {
        all.push_back(std::move(p));
      }
      for (auto& p : temp(v)) {
        all.push_back(std::move(p));
      }
    }
    return all;
  }
  std::vector<Published> cpp_ack(const std::string& cmd_id, const std::string& status = "done") {
    Json payload{{"cmd_id", cmd_id}, {"status", status}, {"limited", false}, {"reason", nullptr}};
    return mqtt(envelope("iot.v1.cmd_ack", kCpp, cpp_seq_++, payload), "cmd/ack");
  }
  std::string rid() {
    auto s = std::to_string(++request_n_);
    return "01JD3T0000000000000000" + std::string(4 - s.size(), '0') + s;
  }
  std::vector<Published> request(Json payload) {
    return mqtt(envelope("iot.v1.request", "app-01", app_seq_++, std::move(payload)), "request");
  }
  std::vector<Published> approve(const std::string& mode = "NORMAL",
                                 const std::string& kind = "manual") {
    Json p{{"request_id", rid()},
           {"kind", kind},
           {"target", "gw-01"},
           {"op", "set_mode"},
           {"args", Json{{"mode", mode}}}};
    if (kind == "schedule") {
      p["schedule"] = Json{{"id", 1}, {"planned_at", "2026-11-26T06:00:00.000Z"}};
    }
    return request(p);
  }
  static std::vector<Json> of(const std::vector<Published>& out, const std::string& suffix) {
    std::vector<Json> result;
    for (const auto& p : out) {
      if (p.topic.ends_with(suffix)) {
        result.push_back(p.message["payload"]);
      }
    }
    return result;
  }
  static std::vector<Json> cmds(const std::vector<Published>& out) { return of(out, "/cmd"); }
  static std::vector<Json> acks(const std::vector<Published>& out) {
    return of(out, "/request/ack");
  }
  static std::vector<Json> events(const std::vector<Published>& out, const std::string& code) {
    std::vector<Json> result;
    for (const auto& e : of(out, "gw-01/event")) {
      if (e["code"] == code) {
        result.push_back(e);
      }
    }
    return result;
  }
  static std::optional<std::string> gw_mode(const std::vector<Published>& out) {
    std::optional<std::string> mode;
    for (const auto& s : of(out, "gw-01/state")) {
      mode = s["mode"].get<std::string>();
    }
    return mode;
  }
};

}  // namespace

TEST_F(CoreRulesTest, CoolingFiresAfterTenSecondsHoldsRenewsAndReleases) {
  boot(kCool);
  auto out = temp(31.0);
  for (int s = 1; s <= 9; ++s) {
    const auto o = hold_temp(31.0, 1);
    EXPECT_TRUE(events(o, "RULE_FIRED").empty()) << s << "秒で発火した";
    EXPECT_TRUE(cmds(o).empty());
  }
  out = advance(Millis{1000});  // 10秒
  ASSERT_EQ(events(out, "RULE_FIRED").size(), 1U);
  const auto fired = events(out, "RULE_FIRED")[0];
  EXPECT_EQ(fired["severity"], "info");  // notify のないルールは info（#47）
  EXPECT_EQ(fired["detail"], (Json{{"rule_id", "cool"}}));
  ASSERT_EQ(cmds(out).size(), 1U);
  const auto cmd = cmds(out)[0];
  EXPECT_EQ(cmd["op"], "set_actuator");
  EXPECT_EQ(cmd["args"], (Json{{"id", "fan_main"}, {"level", 800}}));
  EXPECT_EQ(cmd["priority"], "auto");
  EXPECT_EQ(cmd["lease_s"], 30);
  EXPECT_EQ(cmd["origin"], (Json{{"kind", "rule"}, {"ref", "cool"}}));
  (void)cpp_ack(cmd["cmd_id"]);
  EXPECT_EQ(core->arbiter().owner(c::ActuatorKey{kCpp, "fan_main"})->owner, "rule:cool");
  // 30秒の3分の1ごとに新しい cmd_id で更新する
  out = hold_temp(31.0, 9);
  EXPECT_TRUE(cmds(out).empty());
  out = hold_temp(31.0, 1);
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_NE(cmds(out)[0]["cmd_id"], cmd["cmd_id"]);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  // ヒステリシス：28 までは解除しない。27.9 で解除し release を送る
  out = temp(28.0);
  EXPECT_TRUE(events(out, "RULE_CLEARED").empty());
  out = temp(27.9);
  ASSERT_EQ(events(out, "RULE_CLEARED").size(), 1U);
  EXPECT_EQ(events(out, "RULE_CLEARED")[0]["severity"], "info");
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["op"], "release");
  EXPECT_EQ(core->arbiter().owner(c::ActuatorKey{kCpp, "fan_main"}), nullptr);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = hold_temp(27.0, 15);
  EXPECT_TRUE(cmds(out).empty()) << "解除のあとも更新した";
}

TEST_F(CoreRulesTest, StaleStopsRenewalAndFreshValueResumes) {
  boot(kCoolNow);
  auto out = temp(31.0);
  ASSERT_EQ(cmds(out).size(), 1U);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  // 値が途絶える：state の period_ms 1000 の3倍を超えると stale → 不明。更新（10秒）を送らない
  std::vector<Published> silent;
  for (int s = 0; s < 12; ++s) {
    for (auto& p : advance(Millis{1000})) {
      silent.push_back(std::move(p));
    }
  }
  EXPECT_TRUE(cmds(silent).empty()) << "stale なのにリースを更新した";
  EXPECT_TRUE(events(silent, "RULE_CLEARED").empty());  // 解除はしない
  EXPECT_TRUE(core->rules().unknown(0));
  out = temp(31.0);  // 値が戻ったら hold の指令を送り直す
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["args"]["level"], 800);
}

TEST_F(CoreRulesTest, NonGoodQualityDoesNotFire) {
  boot(kCoolNow);
  EXPECT_TRUE(cmds(reading("temp_air", 40.0, "fault")).empty());
  EXPECT_TRUE(cmds(reading("temp_air", 40.0, "stale")).empty());
  EXPECT_EQ(cmds(temp(40.0)).size(), 1U);
}

constexpr auto kEmergency = R"(
rules:
  - id: heat
    priority: safety
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 35.0}
    hysteresis: 5.0
    then:
      - {action: set_system_mode, mode: EMERGENCY}
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 1000}, lease: 60s, hold: true}
      - {action: notify, level: critical, message: "高温状態が続いている"}
)";

TEST_F(CoreRulesTest, EmergencyRejectsScheduleAndNeedsRecoveryAndApproval) {
  boot(kEmergency);
  auto out = temp(36.0);
  const auto fired = events(out, "RULE_FIRED");
  ASSERT_EQ(fired.size(), 1U);
  EXPECT_EQ(fired[0]["severity"], "critical");
  EXPECT_EQ(fired[0]["detail"]["message"], "高温状態が続いている");
  const auto changed = events(out, "SYSTEM_MODE_CHANGED");
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0]["severity"], "critical");
  EXPECT_EQ(changed[0]["detail"],
            (Json{{"from", "NORMAL"}, {"to", "EMERGENCY"}, {"reason", "heat"}}));
  EXPECT_EQ(gw_mode(out), "EMERGENCY");
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["priority"], "safety");
  EXPECT_EQ(cmds(out)[0]["args"]["level"], 1000);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);

  // EMERGENCY ではスケジュールの要求を拒否する
  out = request(Json{{"request_id", rid()},
                     {"kind", "schedule"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "led_grow"}, {"level", 100}}},
                     {"schedule", Json{{"id", 3}, {"planned_at", "2026-11-26T06:00:00.000Z"}}}});
  ASSERT_EQ(acks(out).size(), 1U);
  EXPECT_EQ(acks(out)[0]["status"], "rejected");
  EXPECT_EQ(acks(out)[0]["reason"], "emergency_mode");
  // 手動の要求は通る
  out = request(Json{{"request_id", rid()},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "led_grow"}, {"level", 100}}}});
  EXPECT_EQ(acks(out)[0]["status"], "accepted");

  // 条件が回復していない承認は unsafe
  out = approve();
  ASSERT_EQ(acks(out).size(), 1U);
  EXPECT_EQ(acks(out)[0]["reason"], "unsafe");
  EXPECT_TRUE(events(out, "SYSTEM_MODE_CHANGED").empty());
  // NORMAL 以外への set_mode と、schedule の承認は not_permitted
  EXPECT_EQ(acks(approve("ALERT"))[0]["reason"], "not_permitted");
  EXPECT_EQ(acks(approve("NORMAL", "schedule"))[0]["reason"], "not_permitted");

  // 回復（35−5＝30 を下回る）しても、承認までは EMERGENCY のまま
  EXPECT_TRUE(events(temp(30.0), "RULE_CLEARED").empty());
  out = temp(29.9);
  ASSERT_EQ(events(out, "RULE_CLEARED").size(), 1U);
  EXPECT_EQ(cmds(out)[0]["op"], "release");
  EXPECT_TRUE(events(out, "SYSTEM_MODE_CHANGED").empty());
  EXPECT_EQ(core->system_mode().mode(), "EMERGENCY");

  out = approve();
  const auto a = acks(out);
  ASSERT_EQ(a.size(), 2U);
  EXPECT_EQ(a[0]["status"], "accepted");
  EXPECT_EQ(a[1]["status"], "done");
  EXPECT_EQ(a[0]["cmd_id"], a[1]["cmd_id"]);
  const auto back = events(out, "SYSTEM_MODE_CHANGED");
  ASSERT_EQ(back.size(), 1U);
  EXPECT_EQ(back[0]["severity"], "info");
  EXPECT_EQ(back[0]["detail"],
            (Json{{"from", "EMERGENCY"}, {"to", "NORMAL"}, {"reason", "approved_by_request"}}));
  EXPECT_EQ(gw_mode(out), "NORMAL");
  // NORMAL のときの承認は done（モードは変えない）
  out = approve();
  EXPECT_EQ(acks(out).back()["status"], "done");
  EXPECT_TRUE(events(out, "SYSTEM_MODE_CHANGED").empty());
}

TEST_F(CoreRulesTest, AutoRulesStopInEmergencyAndResumeAfterApproval) {
  boot(R"(
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 30s, hold: true}
  - id: wet
    priority: safety
    when: {device: mcu-cpp-01, channel: humidity, op: ">=", value: 90.0}
    then:
      - {action: set_system_mode, mode: EMERGENCY}
)");
  auto out = temp(31.0);
  ASSERT_EQ(cmds(out).size(), 1U);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = reading("humidity", 95.0);
  EXPECT_EQ(core->system_mode().mode(), "EMERGENCY");
  // 10秒後の更新を送らない（EMERGENCY では auto のルールのリースを更新しない）
  std::vector<Published> during;
  for (int s = 0; s < 12; ++s) {
    for (auto& p : hold_temp(31.0, 1)) {
      during.push_back(std::move(p));
    }
    (void)reading("humidity", 95.0);
  }
  EXPECT_TRUE(cmds(during).empty());
  (void)reading("humidity", 50.0);
  out = approve();
  ASSERT_EQ(events(out, "SYSTEM_MODE_CHANGED").size(), 1U);
  ASSERT_EQ(cmds(out).size(), 1U);  // 止めていた hold を送り直す
  EXPECT_EQ(cmds(out)[0]["priority"], "auto");
}

TEST_F(CoreRulesTest, AlertReturnsToNormalWhenItsRuleClears) {
  boot(R"(
rules:
  - id: cpp-down
    priority: auto
    when: {device: mcu-cpp-01, status: offline}
    then:
      - {action: set_system_mode, mode: ALERT}
      - {action: notify, level: warning, message: "mcu-cpp-01 が offline"}
)");
  auto out = cpp_state("offline");
  ASSERT_EQ(events(out, "RULE_FIRED").size(), 1U);
  EXPECT_EQ(events(out, "RULE_FIRED")[0]["severity"], "warning");
  const auto alert = events(out, "SYSTEM_MODE_CHANGED");
  ASSERT_EQ(alert.size(), 1U);
  EXPECT_EQ(alert[0]["severity"], "warning");
  EXPECT_EQ(alert[0]["detail"]["to"], "ALERT");
  EXPECT_EQ(gw_mode(out), "ALERT");
  out = cpp_state("online");
  const auto normal = events(out, "SYSTEM_MODE_CHANGED");
  ASSERT_EQ(normal.size(), 1U);
  EXPECT_EQ(normal[0]["severity"], "info");
  EXPECT_EQ(normal[0]["detail"],
            (Json{{"from", "ALERT"}, {"to", "NORMAL"}, {"reason", "cpp-down"}}));
}

TEST_F(CoreRulesTest, ManualReleaseSilencesTheRuleUntilItFiresAgain) {
  boot(kCoolNow);
  auto out = temp(31.0);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = request(Json{{"request_id", rid()},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "release"},
                     {"args", Json{{"id", "fan_main"}}}});
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["op"], "release");
  EXPECT_EQ(cmds(out)[0]["priority"], "manual");
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  EXPECT_TRUE(core->rules().silenced(0));
  out = hold_temp(31.0, 15);
  EXPECT_TRUE(cmds(out).empty()) << "手動で外したのにルールが更新した";
  // 解除しても（所有していないので）release は送らない。再び発火したら送る
  out = temp(27.0);
  EXPECT_EQ(events(out, "RULE_CLEARED").size(), 1U);
  EXPECT_TRUE(cmds(out).empty());
  out = temp(31.0);
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["op"], "set_actuator");
}

TEST_F(CoreRulesTest, RenewalDoesNotOverwriteHigherPriorityControl) {
  boot(kCoolNow);
  auto out = temp(31.0);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = request(Json{{"request_id", rid()},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "fan_main"}, {"level", 300}}},
                     {"lease_s", 15}});
  ASSERT_EQ(cmds(out).size(), 1U);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = hold_temp(31.0, 14);
  EXPECT_TRUE(cmds(out).empty()) << "手動の制御を更新で上書きした";
  // 手動のリースが切れたら、待っていたルールが引き継ぐ（#24）
  out = hold_temp(31.0, 1);
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["priority"], "auto");
  EXPECT_EQ(cmds(out)[0]["args"]["level"], 800);
}

TEST_F(CoreRulesTest, ReconnectAskedForOwnConnectionsOnly) {
  boot(R"(
rules:
  - id: reconnect
    priority: auto
    when: {device: "*", status: offline}
    then:
      - {action: reconnect}
      - {action: notify, level: warning}
)");
  // MQTT のデバイスが offline：作品03 の接続は健全なので開き直さない
  auto raw = mqtt_raw(envelope("iot.v1.state", kCpp, cpp_seq_++,
                               [&] {
                                 auto p = sample("valid/state__online.json")["payload"];
                                 p["status"] = "offline";
                                 return p;
                               }()),
                      "state");
  EXPECT_FALSE(raw.reconnect_uart);
  EXPECT_FALSE(raw.reconnect_mqtt);
  EXPECT_EQ(events(take(raw), "RULE_FIRED").size(), 1U);
  (void)cpp_state("online");
  // UART のデバイスが途絶え、自分の MQTT も切れている：両方を開き直す
  (void)take(core->step(gw::core::UartReceived{u::Decoded{u::Frame{u::State{0, 0x0103, 2, 2000}}},
                                               clock.system_now()}));
  (void)take(core->step(gw::core::MqttDisconnected{}));
  raw = core->step(gw::core::UartLinkDown{});
  EXPECT_TRUE(raw.reconnect_uart);
  EXPECT_TRUE(raw.reconnect_mqtt);
}

TEST_F(CoreRulesTest, ReloadClearsActiveRulesAndSwapsAllThree) {
  boot(kCoolNow);
  auto out = temp(31.0);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  gw::core::Reload reload{gw::adapters::parse_rules_config(R"(
rules:
  - id: humid
    priority: auto
    when: {device: mcu-cpp-01, channel: humidity, op: ">", value: 70.0}
    then:
      - {action: notify, level: info, message: "湿度が高い"}
)",
                                                           "rules", devices()),
                          devices(), c::RequestRules{}};
  const auto raw = core->step(reload);
  out = take(raw);
  ASSERT_EQ(events(out, "RULE_CLEARED").size(), 1U);  // active だったルールの解除処理
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["op"], "release");
  EXPECT_EQ(core->rules().rules().size(), 1U);
  EXPECT_EQ(core->stats().reloads, 1U);
  EXPECT_TRUE(std::any_of(raw.logs.begin(), raw.logs.end(),
                          [](const auto& l) { return l.starts_with("event=reloaded rules=1"); }));
  EXPECT_TRUE(cmds(temp(31.0)).empty());  // 旧いルールは消えた
  EXPECT_EQ(events(reading("humidity", 75.0), "RULE_FIRED").size(), 1U);
  // requests.yaml も入れ替わった（空の許可リスト）
  out = request(Json{{"request_id", rid()},
                     {"kind", "manual"},
                     {"target", kCpp},
                     {"op", "set_actuator"},
                     {"args", Json{{"id", "fan_main"}, {"level", 300}}}});
  EXPECT_EQ(acks(out)[0]["reason"], "not_permitted");
}

TEST_F(CoreRulesTest, QualityCountsAreTelemetryEverySixtySeconds) {
  boot("rules: []");
  core->set_external_counts([] { return gw::core::ExternalCounts{3, 2}; });
  (void)temp(20.0);
  (void)mqtt(envelope("iot.v1.telemetry", kCpp, cpp_seq_ - 1, Json{{"readings", Json::array()}}),
             "telemetry");  // 重複
  std::vector<Published> all;
  for (int s = 0; s < 60; ++s) {
    for (auto& p : advance(Millis{1000})) {
      all.push_back(std::move(p));
    }
  }
  const auto t = of(all, "gw-01/telemetry");
  ASSERT_EQ(t.size(), 1U);
  std::map<std::string, Json> v;
  for (const auto& r : t[0]["readings"]) {
    EXPECT_EQ(r["unit"], "count");
    EXPECT_EQ(r["quality"], "good");
    EXPECT_TRUE(r["value"].is_number_integer());
    v[r["channel"]] = r["value"];
  }
  EXPECT_EQ(v.size(), 8U);
  EXPECT_EQ(v["mqtt_rejected"], 3);
  EXPECT_EQ(v["queue_dropped"], 2);
  EXPECT_EQ(v["mqtt_duplicates"], 1);
  EXPECT_GE(v["stale_transitions"], 1);  // temp_air が途絶えた
}

TEST_F(CoreRulesTest, TimeWindowUsesTheConfiguredOffset) {
  constexpr auto kAfternoon = R"(
rules:
  - id: afternoon
    priority: auto
    when: {time: {from: "15:00", to: "16:00"}}
    then:
      - {action: notify, level: info}
)";
  boot(kAfternoon, true, 0);  // UTC 06:00
  EXPECT_EQ(core->rules().state(0), gw::core::rules::RuleState::kIdle);
  boot(kAfternoon, true, 9 * 60);  // JST 15:00
  EXPECT_EQ(core->rules().state(0), gw::core::rules::RuleState::kActive);
  // 16:00 で解除（1秒ごとの評価）
  std::vector<Published> all;
  for (int s = 0; s < 3600; ++s) {
    for (auto& p : advance(Millis{1000})) {
      all.push_back(std::move(p));
    }
  }
  EXPECT_EQ(events(all, "RULE_CLEARED").size(), 1U);
}

TEST_F(CoreRulesTest, ReloadKeepsUnchangedRulesAndTheirHold) {
  boot(kCoolNow);
  auto out = temp(31.0);
  ASSERT_EQ(cmds(out).size(), 1U);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);
  out = hold_temp(31.0, 3);
  // 同じ cool と、新しいルールを足した rules.yaml を読み直す（#39：同じルールは解除しない）
  const auto same_plus = std::string{kCoolNow} + R"(
  - id: humid
    priority: auto
    when: {device: mcu-cpp-01, channel: humidity, op: ">", value: 70.0}
    then:
      - {action: notify, level: info}
)";
  const auto raw = core->step(gw::core::Reload{
      gw::adapters::parse_rules_config(same_plus, "rules", devices()), devices(), request_rules()});
  out = take(raw);
  EXPECT_TRUE(events(out, "RULE_CLEARED").empty()) << "同じルールを解除した";
  EXPECT_TRUE(cmds(out).empty()) << "同じルールの hold を release した";
  EXPECT_TRUE(std::any_of(raw.logs.begin(), raw.logs.end(), [](const auto& l) {
    return l.starts_with("event=reloaded rules=2 kept=1 cleared=0");
  }));
  EXPECT_EQ(core->rules().state(0), gw::core::rules::RuleState::kActive);
  // リースの更新はそのまま続く（最初の送信から10秒）
  out = hold_temp(31.0, 7);
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_EQ(cmds(out)[0]["args"]["level"], 800);
  (void)cpp_ack(cmds(out)[0]["cmd_id"]);

  // 定義が変わったルール（level 900）は解除して release し、新しい定義で評価し直す
  std::string changed{kCoolNow};
  changed.replace(changed.find("level: 800"), 10, "level: 900");
  const auto raw2 = core->step(gw::core::Reload{
      gw::adapters::parse_rules_config(changed, "rules", devices()), devices(), request_rules()});
  out = take(raw2);
  ASSERT_EQ(events(out, "RULE_CLEARED").size(), 1U);
  ASSERT_EQ(events(out, "RULE_FIRED").size(), 1U);
  ASSERT_EQ(cmds(out).size(), 2U);
  EXPECT_EQ(cmds(out)[0]["op"], "release");
  EXPECT_EQ(cmds(out)[1]["args"]["level"], 900);
}

TEST_F(CoreRulesTest, ReloadKeepsPendingProgress) {
  boot(kCool);  // for: 10s
  (void)temp(31.0);
  (void)hold_temp(31.0, 5);
  (void)take(core->step(gw::core::Reload{
      gw::adapters::parse_rules_config(kCool, "rules", devices()), devices(), request_rules()}));
  EXPECT_EQ(core->rules().state(0), gw::core::rules::RuleState::kPending);
  auto out = hold_temp(31.0, 4);
  EXPECT_TRUE(events(out, "RULE_FIRED").empty());
  out = advance(Millis{1000});  // 最初の成立から10秒（再読込で数え直さない）
  EXPECT_EQ(events(out, "RULE_FIRED").size(), 1U);
}
