// Core の要求と指令（Phase 3）を仮想時計で確かめる：request → 調停 → 指令 → cmd/ack → request/ack。
// 発行した電文はすべて JSON Schema で検証する（SchemaValidator、D6）
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

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

c::RequestRules rules() {
  c::RequestRules r;
  r.allow.push_back({kMcuC, "set_actuator", std::set<std::string>{"fan_main"}});
  r.allow.push_back({kMcuC, "release", std::set<std::string>{"fan_main"}});
  r.allow.push_back({kCpp, "set_actuator", std::set<std::string>{"fan_main", "pump", "led_grow"}});
  r.allow.push_back({kCpp, "release", std::nullopt});
  r.allow.push_back({kCpp, "set_mode", std::nullopt});
  r.allow.push_back({"nope-01", "set_actuator", std::nullopt});
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

class CoreRequestTest : public ::testing::Test {
 protected:
  gw::core::VirtualClock clock{*gw::core::parse_ts("2026-11-26T06:00:00.000Z")};
  gw::core::SequentialIdGenerator ids{clock};
  gw::core::Core core{gw::core::CoreConfig{"home",
                                           "gw-01",
                                           u::load_profile(GW_PROFILES_DIR, "gw-01"),
                                           u::load_profile(GW_PROFILES_DIR, kMcuC),
                                           "0.1.0",
                                           "0badcafe",
                                           {u::load_profile(GW_PROFILES_DIR, kCpp)},
                                           rules(),
                                           {}},
                      clock, ids};
  gw::adapters::SchemaValidator validator{std::string{GW_IOTSPEC_DIR} + "/schemas"};
  std::uint32_t app_seq_ = 0;
  std::uint32_t cpp_seq_ = 0;
  std::uint16_t uart_seq_ = 0;

  void SetUp() override {
    (void)take(core.start());
    cpp_state("online");
    (void)take(rx(u::State{uart_seq_++, 0x0103, 2, 2000}));
    ASSERT_TRUE(core.device_online(kCpp));
    ASSERT_TRUE(core.device_online(kMcuC));
  }

  // 発行した電文をすべて Schema で検証して返す
  std::vector<Published> take(const gw::core::Outputs& out) {
    std::vector<Published> result;
    for (const auto& m : out.mqtt) {
      const auto v = validator.validate(m.payload);
      EXPECT_TRUE(v.ok) << m.topic << " " << v.reason << " " << v.detail;
      result.push_back(Published{m.topic, Json::parse(m.payload)});
    }
    return result;
  }
  gw::core::Outputs rx(const u::Frame& frame) {
    return core.step(gw::core::UartReceived{u::Decoded{frame}, clock.system_now()});
  }
  Json envelope(const std::string& schema, const std::string& device, std::uint32_t seq,
                Json payload) {
    return Json{{"schema", schema},      {"msg_id", "01JD3R0A5K7M2N4P6R8T0V2X4Y"},
                {"device_id", device},   {"seq", seq},
                {"boot_id", "0000000a"}, {"ts", gw::core::format_ts(clock.system_now())},
                {"ts_source", "device"}, {"payload", std::move(payload)}};
  }
  gw::core::Outputs mqtt(const Json& message, const std::string& kind) {
    return core.step(gw::core::MqttReceived{"t", message, kind, clock.system_now(), false});
  }
  void cpp_state(const std::string& status) {
    auto payload = sample("valid/state__online.json")["payload"];
    payload["status"] = status;
    (void)take(mqtt(envelope("iot.v1.state", kCpp, cpp_seq_++, payload), "state"));
  }
  std::vector<Published> request(const Json& payload) {
    return take(mqtt(envelope("iot.v1.request", "app-01", app_seq_++, payload), "request"));
  }
  std::vector<Published> cpp_ack(const std::string& cmd_id, const std::string& status,
                                 const Json& reason = nullptr) {
    Json payload{{"cmd_id", cmd_id}, {"status", status}, {"limited", false}, {"reason", reason}};
    return take(mqtt(envelope("iot.v1.cmd_ack", kCpp, cpp_seq_++, payload), "cmd/ack"));
  }
  std::vector<Published> advance(Millis d) {
    clock.advance(d);
    return take(core.on_timer(clock.steady_now()));
  }
  static Json set(const std::string& id, const std::string& target, const std::string& actuator,
                  int level, const std::string& kind = "manual", int lease_s = 30) {
    Json p{{"request_id", id},
           {"kind", kind},
           {"target", target},
           {"op", "set_actuator"},
           {"args", Json{{"id", actuator}, {"level", level}}},
           {"lease_s", lease_s}};
    if (kind == "schedule") {
      p["schedule"] = Json{{"id", 12}, {"planned_at", "2026-11-26T06:00:00.000Z"}};
    }
    return p;
  }
  static std::vector<Json> of(const std::vector<Published>& out, const std::string& suffix) {
    std::vector<Json> result;
    for (const auto& p : out) {
      if (p.topic.size() >= suffix.size() &&
          p.topic.compare(p.topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
        result.push_back(p.message["payload"]);
      }
    }
    return result;
  }
  static std::vector<Json> acks(const std::vector<Published>& out) {
    return of(out, "/request/ack");
  }
  static std::vector<Json> cmds(const std::vector<Published>& out) { return of(out, "/cmd"); }
};

// ULID の形（26文字）。テストの request_id に使う
std::string rid(int n) {
  auto s = std::to_string(n);
  return "01JD3S0000000000000000" + std::string(4 - s.size(), '0') + s;
}

}  // namespace

TEST_F(CoreRequestTest, MqttRequestAcceptedThenDone) {
  const auto out = request(set(rid(1), kCpp, "pump", 1000, "manual", 20));
  ASSERT_EQ(acks(out).size(), 1U);
  const auto accepted = acks(out)[0];
  EXPECT_EQ(accepted["status"], "accepted");
  EXPECT_TRUE(accepted["reason"].is_null());
  ASSERT_EQ(cmds(out).size(), 1U);
  const auto cmd = cmds(out)[0];
  EXPECT_EQ(cmd["cmd_id"], accepted["cmd_id"]);
  EXPECT_EQ(cmd["priority"], "manual");
  EXPECT_EQ(cmd["lease_s"], 20);
  EXPECT_EQ(cmd["origin"], (Json{{"kind", "request"}, {"ref", rid(1)}}));
  for (const auto& p : out) {
    if (p.topic.ends_with("/request/ack")) {
      EXPECT_EQ(p.topic, "iot/v1/home/app-01/request/ack");
      EXPECT_EQ(p.message["device_id"], "gw-01");
    } else if (p.topic.ends_with("/cmd")) {
      EXPECT_EQ(p.topic, "iot/v1/home/mcu-cpp-01/cmd");
    }
  }
  const auto done = cpp_ack(cmd["cmd_id"], "done");
  ASSERT_EQ(acks(done).size(), 1U);
  EXPECT_EQ(acks(done)[0]["status"], "done");
  EXPECT_EQ(acks(done)[0]["cmd_id"], cmd["cmd_id"]);
  EXPECT_EQ(core.arbiter().owner(c::ActuatorKey{kCpp, "pump"})->owner, "request:" + rid(1));
}

TEST_F(CoreRequestTest, NonActuatorOpIsNotArbitrated) {
  const auto out = request(Json{{"request_id", rid(2)},
                                {"kind", "manual"},
                                {"target", kCpp},
                                {"op", "set_mode"},
                                {"args", Json{{"mode", "manual"}}}});
  ASSERT_EQ(cmds(out).size(), 1U);
  EXPECT_FALSE(cmds(out)[0].contains("lease_s"));
  EXPECT_EQ(core.arbiter().next_expiry(), std::nullopt);
}

TEST_F(CoreRequestTest, DuplicateRequestReturnsCurrentStateWithoutNewCommand) {
  const auto first = request(set(rid(3), kCpp, "fan_main", 500));
  const auto cmd_id = cmds(first)[0]["cmd_id"];
  const auto again = request(set(rid(3), kCpp, "fan_main", 500));
  EXPECT_TRUE(cmds(again).empty());
  ASSERT_EQ(acks(again).size(), 1U);
  EXPECT_EQ(acks(again)[0]["status"], "accepted");
  EXPECT_EQ(acks(again)[0]["cmd_id"], cmd_id);
  (void)cpp_ack(cmd_id, "done");
  const auto after = request(set(rid(3), kCpp, "fan_main", 500));
  EXPECT_EQ(acks(after)[0]["status"], "done");
  EXPECT_EQ(acks(after)[0]["cmd_id"], cmd_id);
  EXPECT_EQ(core.requests().stats().duplicates, 2U);
}

TEST_F(CoreRequestTest, RequestIdIsForgottenAfterOneHour) {
  const auto first = request(set(rid(4), kCpp, "fan_main", 500, "manual", 600));
  (void)cpp_ack(cmds(first)[0]["cmd_id"], "done");
  (void)advance(Millis{3'599'999});
  EXPECT_TRUE(cmds(request(set(rid(4), kCpp, "fan_main", 500))).empty());
  (void)advance(Millis{1});
  EXPECT_EQ(core.requests().remembered(), 0U);
  cpp_state("online");
  const auto later = request(set(rid(4), kCpp, "fan_main", 500));
  EXPECT_EQ(cmds(later).size(), 1U);  // 忘れたので新しい要求として扱う
}

TEST_F(CoreRequestTest, NotPermittedUnknownTargetAndOffline) {
  auto out = request(set(rid(5), kCpp, "buzzer", 500));
  ASSERT_EQ(acks(out).size(), 1U);
  EXPECT_EQ(acks(out)[0]["status"], "rejected");
  EXPECT_EQ(acks(out)[0]["reason"], "not_permitted");
  EXPECT_FALSE(acks(out)[0].contains("cmd_id"));
  EXPECT_TRUE(cmds(out).empty());

  out = request(set(rid(6), "nope-01", "fan_main", 500));  // 許可されていても未登録
  EXPECT_EQ(acks(out)[0]["reason"], "not_permitted");

  cpp_state("offline");
  out = request(set(rid(7), kCpp, "fan_main", 500));
  EXPECT_EQ(acks(out)[0]["status"], "rejected");
  EXPECT_EQ(acks(out)[0]["reason"], "target_offline");
  EXPECT_TRUE(cmds(out).empty());
}

TEST_F(CoreRequestTest, LowerPriorityIsRejected) {
  (void)request(set(rid(8), kCpp, "fan_main", 500, "manual", 60));
  const auto out = request(set(rid(9), kCpp, "fan_main", 100, "schedule", 60));
  EXPECT_EQ(acks(out)[0]["status"], "rejected");
  EXPECT_EQ(acks(out)[0]["reason"], "lower_priority");
  EXPECT_TRUE(cmds(out).empty());
}

TEST_F(CoreRequestTest, ReleaseHandsOverToTheWaitingSchedule) {
  const auto sched = request(set(rid(10), kCpp, "fan_main", 300, "schedule", 60));
  (void)cpp_ack(cmds(sched)[0]["cmd_id"], "done");
  const auto manual = request(set(rid(11), kCpp, "fan_main", 900, "manual", 60));
  (void)cpp_ack(cmds(manual)[0]["cmd_id"], "done");
  (void)advance(Millis{10'000});
  const auto rel = request(Json{{"request_id", rid(12)},
                                {"kind", "manual"},
                                {"target", kCpp},
                                {"op", "release"},
                                {"args", Json{{"id", "fan_main"}}}});
  ASSERT_EQ(cmds(rel).size(), 1U);
  const auto handover = cmds(rel)[0];
  EXPECT_EQ(handover["op"], "set_actuator");  // 待っていた schedule が引き継ぐ（#24）
  EXPECT_EQ(handover["args"]["level"], 300);
  EXPECT_EQ(handover["priority"], "schedule");
  EXPECT_EQ(handover["lease_s"], 50);  // 残りのリース
  EXPECT_EQ(acks(rel)[0]["cmd_id"], handover["cmd_id"]);
  const auto done = cpp_ack(handover["cmd_id"], "done");
  EXPECT_EQ(acks(done)[0]["status"], "done");
  EXPECT_EQ(core.arbiter().owner(c::ActuatorKey{kCpp, "fan_main"})->owner, "request:" + rid(10));
}

TEST_F(CoreRequestTest, ReleaseWithoutWaitingSendsRelease) {
  const auto set_out = request(set(rid(13), kCpp, "led_grow", 800));
  (void)cpp_ack(cmds(set_out)[0]["cmd_id"], "done");
  const auto rel = request(Json{{"request_id", rid(14)},
                                {"kind", "manual"},
                                {"target", kCpp},
                                {"op", "release"},
                                {"args", Json{{"id", "led_grow"}}}});
  ASSERT_EQ(cmds(rel).size(), 1U);
  EXPECT_EQ(cmds(rel)[0]["op"], "release");
  EXPECT_EQ(core.arbiter().owner(c::ActuatorKey{kCpp, "led_grow"}), nullptr);
  // schedule からの解放は manual の所有に負ける
  (void)request(set(rid(15), kCpp, "led_grow", 800));
  const auto low =
      request(Json{{"request_id", rid(16)},
                   {"kind", "schedule"},
                   {"target", kCpp},
                   {"op", "release"},
                   {"args", Json{{"id", "led_grow"}}},
                   {"schedule", Json{{"id", 3}, {"planned_at", "2026-11-26T06:00:00.000Z"}}}});
  EXPECT_EQ(acks(low)[0]["reason"], "lower_priority");
}

TEST_F(CoreRequestTest, DeviceRejectionIsFailedWithItsReasonAndFreesTheActuator) {
  const auto out = request(set(rid(17), kCpp, "pump", 1000));
  const auto rejected = cpp_ack(cmds(out)[0]["cmd_id"], "rejected", "unsafe");
  ASSERT_EQ(acks(rejected).size(), 1U);
  EXPECT_EQ(acks(rejected)[0]["status"], "failed");
  EXPECT_EQ(acks(rejected)[0]["reason"], "unsafe");
  EXPECT_TRUE(of(rejected, "/event").empty());  // COMMAND_FAILED は FAILED・TIMED_OUT だけ
  EXPECT_EQ(core.arbiter().owner(c::ActuatorKey{kCpp, "pump"}), nullptr);
  // 主張が外れたので低い優先度でも通る
  const auto sched = request(set(rid(18), kCpp, "pump", 500, "schedule"));
  EXPECT_EQ(acks(sched)[0]["status"], "accepted");
}

TEST_F(CoreRequestTest, TimeoutIsFailedTargetOfflineWithCommandFailed) {
  const auto out = request(set(rid(19), kCpp, "fan_main", 500));
  const auto cmd_id = cmds(out)[0]["cmd_id"];
  int resends = 0;
  std::vector<Published> last;
  for (int i = 0; i < 4; ++i) {
    last = advance(Millis{2000});
    for (const auto& c1 : cmds(last)) {
      EXPECT_EQ(c1["cmd_id"], cmd_id);
      ++resends;
    }
  }
  EXPECT_EQ(resends, 3);
  ASSERT_EQ(acks(last).size(), 1U);
  EXPECT_EQ(acks(last)[0]["status"], "failed");
  EXPECT_EQ(acks(last)[0]["reason"], "target_offline");
  const auto events = of(last, "/event");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0]["code"], "COMMAND_FAILED");
  EXPECT_EQ(events[0]["severity"], "warning");
  EXPECT_EQ(events[0]["detail"]["cmd_id"], cmd_id);
  EXPECT_EQ(events[0]["detail"]["target"], kCpp);
  EXPECT_EQ(events[0]["detail"]["reason"], "timeout");
  EXPECT_EQ(core.stats().command_failed_events, 1U);
}

TEST_F(CoreRequestTest, DeviceFailedAfterAcceptedPublishesCommandFailed) {
  const auto out = request(set(rid(20), kCpp, "fan_main", 500));
  const auto cmd_id = cmds(out)[0]["cmd_id"];
  EXPECT_TRUE(acks(cpp_ack(cmd_id, "accepted")).empty());
  const auto failed = cpp_ack(cmd_id, "failed", "internal");
  EXPECT_EQ(acks(failed)[0]["status"], "failed");
  ASSERT_EQ(of(failed, "/event").size(), 1U);
  EXPECT_EQ(of(failed, "/event")[0]["detail"]["reason"], "internal");
}

TEST_F(CoreRequestTest, UartRequestGoesThroughCmdAndAck) {
  const auto out = core.step(gw::core::MqttReceived{
      "t", envelope("iot.v1.request", "app-01", app_seq_++, set(rid(21), kMcuC, "fan_main", 250)),
      "request", clock.system_now(), false});
  const auto published = take(out);
  ASSERT_EQ(acks(published).size(), 1U);
  EXPECT_TRUE(cmds(published).empty());  // UART のデバイスへは MQTT の cmd を出さない
  std::vector<u::Cmd> frames;
  for (const auto& tx : out.uart) {
    if (const auto* f = std::get_if<u::Cmd>(&tx.frame)) {
      frames.push_back(*f);
    }
  }
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].op, 0x01);  // SET_FAN_OVERRIDE
  EXPECT_EQ(frames[0].ttl_s, 30);
  EXPECT_EQ(frames[0].args, (std::vector<std::int32_t>{250}));
  const auto ack = take(rx(u::Ack{0, frames[0].cmd_ref, 1, 0, 0, 250}));
  const auto cmd_acks = of(ack, "/cmd/ack");
  ASSERT_EQ(cmd_acks.size(), 1U);  // mcu-c-01 名義の cmd/ack
  EXPECT_EQ(cmd_acks[0]["cmd_id"], acks(published)[0]["cmd_id"]);
  ASSERT_EQ(acks(ack).size(), 1U);
  EXPECT_EQ(acks(ack)[0]["status"], "done");
}

TEST_F(CoreRequestTest, LeaseExpiryHandsOverWithoutRelease) {
  const auto sched = request(set(rid(22), kCpp, "fan_main", 300, "schedule", 60));
  (void)cpp_ack(cmds(sched)[0]["cmd_id"], "done");
  const auto manual = request(set(rid(23), kCpp, "fan_main", 900, "manual", 20));
  (void)cpp_ack(cmds(manual)[0]["cmd_id"], "done");
  EXPECT_EQ(core.next_deadline() <= clock.steady_now() + Millis{20'000}, true);
  const auto expired = advance(Millis{20'000});
  ASSERT_EQ(cmds(expired).size(), 1U);  // manual のリースが切れて schedule が引き継ぐ
  EXPECT_EQ(cmds(expired)[0]["priority"], "schedule");
  EXPECT_EQ(cmds(expired)[0]["lease_s"], 40);
  EXPECT_TRUE(acks(expired).empty());
}
