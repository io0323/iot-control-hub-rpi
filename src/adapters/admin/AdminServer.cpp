#include "adapters/admin/AdminServer.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <set>

#include <spdlog/spdlog.h>

namespace gw::adapters {

namespace {

using core::Json;

const std::set<std::string, std::less<>> kCommands{"devices", "rules", "stats", "approve",
                                                   "reload",  "cmd",   "diag"};
constexpr std::size_t kMaxLine = std::size_t{64} * 1024;
constexpr int kPollMs = 200;
constexpr mode_t kSocketMode = 0660;
constexpr auto kDiagPoll = std::chrono::milliseconds{200};

Json error_reply(const std::string& message) {
  return Json{{"ok", false}, {"error", message}};
}

void close_fd(int& fd) {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

}  // namespace

std::variant<core::AdminRequest, std::string> parse_admin_request(const std::string& line) {
  const auto j = Json::parse(line, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return std::string{"JSON の object でない"};
  }
  const auto cmd = j.value("cmd", "");
  if (!kCommands.contains(cmd)) {
    return "知らないコマンド: '" + cmd + "'";
  }
  core::AdminRequest r;
  r.cmd = cmd;
  if (j.contains("args")) {
    if (!j["args"].is_object()) {
      return std::string{"args が object でない"};
    }
    r.args = j["args"];
  }
  if (cmd == "diag" && (!r.args.contains("device") || !r.args["device"].is_string())) {
    return std::string{"diag には args.device が要る"};
  }
  if (cmd == "cmd") {
    if (!r.args.contains("target") || !r.args["target"].is_string() || !r.args.contains("op") ||
        !r.args["op"].is_string()) {
      return std::string{"cmd には args.target と args.op が要る"};
    }
    if (r.args.contains("args") && !r.args["args"].is_object()) {
      return std::string{"cmd の args.args が object でない"};
    }
    if (r.args.contains("lease_s") && !r.args["lease_s"].is_number_integer()) {
      return std::string{"cmd の args.lease_s が整数でない"};
    }
  }
  return r;
}

AdminServer::AdminServer(std::filesystem::path socket, Hooks hooks)
    : socket_(std::move(socket)), hooks_(std::move(hooks)) {
}

AdminServer::~AdminServer() {
  stop();
}

bool AdminServer::start() {
  sockaddr_un addr{};
  const auto path = socket_.string();
  if (path.size() >= sizeof(addr.sun_path)) {
    error_ = "ソケットのパスが長すぎる: " + path;
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(socket_.parent_path(), ec);
  (void)::unlink(path.c_str());  // 前回の残り（同じパスのソケット）
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    error_ = std::string{"socket: "} + std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||  // NOLINT
      ::chmod(path.c_str(), kSocketMode) != 0 || ::listen(listen_fd_, 4) != 0) {
    error_ = path + ": " + std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
    close_fd(listen_fd_);
    return false;
  }
  std::array<int, 2> fds{};
  if (::pipe(fds.data()) != 0) {
    error_ = std::string{"pipe: "} + std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
    close_fd(listen_fd_);
    return false;
  }
  wake_read_ = fds[0];
  wake_write_ = fds[1];
  thread_ = std::thread([this] { run(); });
  return true;
}

void AdminServer::stop() {
  if (!thread_.joinable()) {
    return;
  }
  stopping_ = true;
  const char byte = 1;
  (void)::write(wake_write_, &byte, 1);
  cv_.notify_all();
  thread_.join();
  close_fd(listen_fd_);
  close_fd(wake_read_);
  close_fd(wake_write_);
  (void)::unlink(socket_.c_str());
}

void AdminServer::run() {
  while (!stopping_) {
    std::array<pollfd, 2> fds{pollfd{listen_fd_, POLLIN, 0}, pollfd{wake_read_, POLLIN, 0}};
    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("event=admin_poll_failed errno={}", errno);
      return;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd >= 0) {
        serve(fd);  // 1つずつ処理する（管理用なので同時に1つで足りる）
        (void)::close(fd);
      }
    }
  }
}

void AdminServer::serve(int fd) {
  std::string line;
  std::array<char, 4096> buf{};
  while (!stopping_ && line.find('\n') == std::string::npos && line.size() < kMaxLine) {
    pollfd p{fd, POLLIN, 0};
    const int rc = ::poll(&p, 1, kPollMs * 10);
    if (rc <= 0) {
      return;  // 来ない
    }
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    line.append(buf.data(), static_cast<std::size_t>(n));
  }
  if (const auto pos = line.find('\n'); pos != std::string::npos) {
    line.resize(pos);
  }
  const auto reply = handle(line).dump() + "\n";
  std::size_t sent = 0;
  while (sent < reply.size()) {
    const auto n = ::write(fd, reply.data() + sent, reply.size() - sent);
    if (n <= 0) {
      return;
    }
    sent += static_cast<std::size_t>(n);
  }
}

std::optional<Json> AdminServer::ask(core::AdminRequest request,
                                     std::chrono::milliseconds timeout) {
  std::uint64_t id = 0;
  {
    const std::lock_guard lock(mutex_);
    id = next_id_++;
    pending_[id] = std::nullopt;
  }
  request.id = id;
  if (!hooks_.submit || !hooks_.submit(std::move(request))) {
    const std::lock_guard lock(mutex_);
    pending_.erase(id);
    return error_reply("core が受け付けない（終了中）");
  }
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, timeout, [&] { return stopping_ || pending_[id].has_value(); });
  auto reply = std::move(pending_[id]);
  pending_.erase(id);
  return reply;
}

void AdminServer::deliver(const core::AdminReply& reply) {
  {
    const std::lock_guard lock(mutex_);
    const auto it = pending_.find(reply.id);
    if (it == pending_.end()) {
      return;  // 待つのをやめた要求（時間切れ）
    }
    it->second = reply.body;
  }
  cv_.notify_all();
}

Json AdminServer::handle(const std::string& line) {
  auto parsed = parse_admin_request(line);
  if (const auto* err = std::get_if<std::string>(&parsed)) {
    return error_reply(*err);
  }
  auto request = std::get<core::AdminRequest>(std::move(parsed));
  spdlog::info("event=admin_request cmd={}", request.cmd);
  if (request.cmd == "reload") {
    return hooks_.reload ? hooks_.reload() : error_reply("再読込できない");
  }
  if (request.cmd == "cmd") {
    auto reply = ask(std::move(request), kCmdTimeout);
    // 最終結果が来ないまま時間切れ：指令は続いている
    return reply
               ? *reply
               : Json{{"ok", true}, {"result", Json{{"status", "accepted"}, {"note", "timeout"}}}};
  }
  const auto cmd = request.cmd;
  const auto device = request.args.value("device", "");
  auto reply = ask(std::move(request), kReplyTimeout);
  if (!reply) {
    return error_reply("core が応答しない");
  }
  if (cmd == "stats" && reply->value("ok", false) && hooks_.extra_stats) {
    (*reply)["result"]["process"] = hooks_.extra_stats();
  }
  if (cmd == "diag" && reply->value("ok", false) && hooks_.read_diags) {
    // GET_DIAG を送った後に届いた DIAG が履歴に書かれるのを待つ（DIAG → ACK の順。#37）
    const auto since = (*reply)["result"].value("requested_ms", std::int64_t{0});
    const auto deadline = std::chrono::steady_clock::now() + kDiagWait;
    while (std::chrono::steady_clock::now() < deadline && !stopping_) {
      auto rows = hooks_.read_diags(device, since);
      if (rows.is_array() && !rows.empty()) {
        (*reply)["result"]["status"] = "received";
        (*reply)["result"]["diag"] = rows.front();
        return *reply;
      }
      std::this_thread::sleep_for(kDiagPoll);
    }
    return error_reply("DIAG が届かない（" + device + "）");
  }
  return *reply;
}

}  // namespace gw::adapters
