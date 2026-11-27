// gwctl の管理ソケット（AdminServer）：要求の解析、core への受け渡しと応答、reload、diag の待ち、
// stats への追加、時間切れ、UNIX ドメインソケット越しのやり取り
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>

#include "adapters/admin/AdminServer.h"

using gw::adapters::AdminServer;
using gw::adapters::parse_admin_request;
using gw::core::Json;

namespace {

std::string error_of(const std::string& line) {
  const auto r = parse_admin_request(line);
  const auto* e = std::get_if<std::string>(&r);
  return e == nullptr ? "" : *e;
}

// core の代わり：受けた要求に別のスレッドから応答する
struct FakeCore {
  AdminServer* server = nullptr;
  std::vector<gw::core::AdminRequest> seen;
  std::function<Json(const gw::core::AdminRequest&)> answer = [](const auto&) {
    return Json{{"ok", true}, {"result", Json{{"echo", true}}}};
  };
  bool submit(const gw::core::AdminRequest& r) {
    seen.push_back(r);
    std::thread([this, r] {
      const auto body = answer(r);
      if (!body.is_null()) {
        server->deliver(gw::core::AdminReply{r.id, body});
      }
    }).detach();
    return true;
  }
};

std::filesystem::path socket_path() {
  return std::filesystem::path{"/tmp"} / ("gwctl-test-" + std::to_string(::getpid()) + ".sock");
}

}  // namespace

TEST(AdminRequest, Parse) {
  const auto ok = parse_admin_request(
      R"({"cmd":"cmd","args":{"target":"mcu-cpp-01","op":"set_actuator","args":{"id":"fan_main","level":3},"lease_s":60}})");
  ASSERT_TRUE(std::holds_alternative<gw::core::AdminRequest>(ok));
  EXPECT_EQ(std::get<gw::core::AdminRequest>(ok).args["args"]["level"], 3);
  EXPECT_TRUE(
      std::holds_alternative<gw::core::AdminRequest>(parse_admin_request(R"({"cmd":"devices"})")));
  EXPECT_NE(error_of("not json").find("JSON"), std::string::npos);
  EXPECT_NE(error_of("[1]").find("JSON"), std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"format"})").find("format"), std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"rules","args":1})").find("args"), std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"diag"})").find("device"), std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"cmd","args":{"target":"x"}})").find("op"), std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"cmd","args":{"target":"x","op":"get_status","args":[]}})")
                .find("args.args"),
            std::string::npos);
  EXPECT_NE(error_of(R"({"cmd":"cmd","args":{"target":"x","op":"get_status","lease_s":"1"}})")
                .find("lease_s"),
            std::string::npos);
}

TEST(AdminServer, HandlesRequestsThroughCoreAndLocalHooks) {
  FakeCore core;
  std::atomic<int> reloads{0};
  AdminServer server(
      socket_path(),
      AdminServer::Hooks{[&](const gw::core::AdminRequest& r) { return core.submit(r); },
                         [&] {
                           ++reloads;
                           return Json{{"ok", true}, {"result", Json{{"rules", 3}}}};
                         },
                         [](const std::string& device, std::int64_t since) {
                           return Json::array({Json{{"device_id", device}, {"since", since}}});
                         },
                         [] {
                           return Json{{"history", Json{{"written", 5}}}};
                         }});
  core.server = &server;
  EXPECT_EQ(server.handle(R"({"cmd":"rules"})")["result"]["echo"], true);
  EXPECT_EQ(core.seen.back().cmd, "rules");
  EXPECT_EQ(server.handle(R"({"cmd":"reload"})")["result"]["rules"], 3);
  EXPECT_EQ(reloads, 1);
  EXPECT_FALSE(server.handle("oops")["ok"]);
  // stats：core の応答に core の外の数を足す
  EXPECT_EQ(server.handle(R"({"cmd":"stats"})")["result"]["process"]["history"]["written"], 5);
  // diag：core が sent を返したら、履歴から DIAG を読む
  core.answer = [](const auto&) {
    return Json{{"ok", true}, {"result", Json{{"status", "sent"}, {"requested_ms", 42}}}};
  };
  const auto diag = server.handle(R"({"cmd":"diag","args":{"device":"mcu-c-01"}})");
  EXPECT_EQ(diag["result"]["status"], "received");
  EXPECT_EQ(diag["result"]["diag"]["since"], 42);
  // core が誤りを返したら、そのまま返す
  core.answer = [](const auto&) {
    return Json{{"ok", false}, {"error", "UART のデバイスではない"}};
  };
  EXPECT_FALSE(server.handle(R"({"cmd":"diag","args":{"device":"x"}})")["ok"]);
}

TEST(AdminServer, TimesOutWhenCoreDoesNotAnswer) {
  FakeCore core;
  core.answer = [](const auto&) { return Json(nullptr); };  // 応答しない
  AdminServer server(socket_path(), AdminServer::Hooks{[&](const gw::core::AdminRequest& r) {
                                                         return core.submit(r);
                                                       },
                                                       nullptr, nullptr, nullptr});
  core.server = &server;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(server.handle(R"({"cmd":"devices"})")["ok"]);
  EXPECT_GE(std::chrono::steady_clock::now() - start, AdminServer::kReplyTimeout);
  // 受け付けられない（終了中）
  AdminServer closed(socket_path(),
                     AdminServer::Hooks{[](const gw::core::AdminRequest&) { return false; },
                                        nullptr, nullptr, nullptr});
  EXPECT_FALSE(closed.handle(R"({"cmd":"devices"})")["ok"]);
  EXPECT_FALSE(closed.handle(R"({"cmd":"reload"})")["ok"]);
}

TEST(AdminServer, ServesOneLineJsonOverAUnixSocket) {
  FakeCore core;
  const auto path = socket_path();
  AdminServer server(
      path, AdminServer::Hooks{[&](const gw::core::AdminRequest& r) { return core.submit(r); },
                               nullptr, nullptr, nullptr});
  core.server = &server;
  ASSERT_TRUE(server.start()) << server.error();
  struct stat st {};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0660U);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);  // NOLINT
  const std::string req = R"({"cmd":"devices"})"
                          "\n";
  ASSERT_EQ(::write(fd, req.data(), req.size()), static_cast<ssize_t>(req.size()));
  std::string reply;
  char buf[256];
  while (reply.find('\n') == std::string::npos) {
    const auto n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    reply.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  EXPECT_EQ(Json::parse(reply)["result"]["echo"], true);
  server.stop();
  EXPECT_FALSE(std::filesystem::exists(path));
  // 作れないパス
  AdminServer bad("/nonexistent-dir-gw/" + std::string(200, 'x') + ".sock", AdminServer::Hooks{});
  EXPECT_FALSE(bad.start());
  EXPECT_FALSE(bad.error().empty());
}
