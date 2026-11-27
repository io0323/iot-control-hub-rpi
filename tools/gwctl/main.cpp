// gwctl：iot-gw の管理コマンド（基本設計 FR-15、Phase 5）。UNIX ドメインソケットに1行の JSON を送り、
// 応答を人が読む表か（--json なら）JSON のまま出す。
//
//   gwctl [--socket PATH] [--json] devices
//   gwctl ... rules | stats | reload | approve
//   gwctl ... cmd <target> <op> [key=value ...] [--lease N]   # 優先度 manual、origin gwctl
//   gwctl ... diag <device>                                    # GET_DIAG を送り、届いた DIAG を表示
//
// ソケットの既定は環境変数 GWCTL_SOCKET、無ければ /run/iot-gw/gwctl.sock。
// 終了コード：0 成功、1 iot-gw が誤りを返した、2 使い方・接続の誤り
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::ordered_json;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr const char* kDefaultSocket = "/run/iot-gw/gwctl.sock";

void usage() {
  std::cerr << "使い方: gwctl [--socket PATH] [--json] "
               "devices|rules|stats|reload|approve|cmd <target> <op> [key=value ...] [--lease N]|"
               "diag <device>\n";
}

// "123" は整数、"1.5" は数、ほかは文字列
Json value_of(const std::string& text) {
  char* end = nullptr;
  errno = 0;
  const long long n = std::strtoll(text.c_str(), &end, 10);
  if (errno == 0 && end != text.c_str() && *end == '\0') {
    return n;
  }
  end = nullptr;
  const double d = std::strtod(text.c_str(), &end);
  if (end != text.c_str() && *end == '\0') {
    return d;
  }
  return text;
}

std::optional<std::string> send_request(const std::string& path, const std::string& line) {
  sockaddr_un addr{};
  if (path.size() >= sizeof(addr.sun_path)) {
    std::cerr << "gwctl: ソケットのパスが長すぎる: " << path << "\n";
    return std::nullopt;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {      // NOLINT
    std::cerr << "gwctl: " << path << " につながらない: " << std::strerror(errno)  // NOLINT
              << "\n";
    (void)::close(fd);
    return std::nullopt;
  }
  const auto request = line + "\n";
  if (::write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
    (void)::close(fd);
    return std::nullopt;
  }
  std::string reply;
  std::array<char, 4096> buf{};
  while (reply.find('\n') == std::string::npos) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    reply.append(buf.data(), static_cast<std::size_t>(n));
  }
  (void)::close(fd);
  if (const auto pos = reply.find('\n'); pos != std::string::npos) {
    reply.resize(pos);
  }
  return reply;
}

std::string text(const Json& v) {
  if (v.is_null()) {
    return "-";
  }
  return v.is_string() ? v.get<std::string>() : v.dump();
}

void print_devices(const Json& list) {
  std::cout << std::left << std::setw(14) << "DEVICE" << std::setw(10) << "STATUS" << std::setw(10)
            << "TRANSPORT"
            << "CHANNELS\n";
  for (const auto& d : list) {
    std::string channels;
    for (const auto& c : d["channels"]) {
      channels += (channels.empty() ? "" : " ") + c.value("channel", "") + "=" + text(c["value"]) +
                  (c.value("stale", false) ? "(stale)" : "");
    }
    std::cout << std::left << std::setw(14) << d.value("device_id", "") << std::setw(10)
              << d.value("status", "") << std::setw(10) << d.value("transport", "")
              << (channels.empty() ? "-" : channels) << "\n";
  }
}

void print_rules(const Json& r) {
  std::cout << "mode: " << r.value("mode", "") << "\n";
  std::cout << std::left << std::setw(32) << "RULE" << std::setw(10) << "PRIORITY" << std::setw(10)
            << "STATE"
            << "FLAGS\n";
  for (const auto& rule : r["rules"]) {
    std::string flags;
    if (rule.value("unknown", false)) {
      flags += "unknown ";
    }
    if (rule.value("silenced", false)) {
      flags += "silenced";
    }
    std::cout << std::left << std::setw(32) << rule.value("id", "") << std::setw(10)
              << rule.value("priority", "") << std::setw(10) << rule.value("state", "")
              << (flags.empty() ? "-" : flags) << "\n";
  }
}

void print_flat(const Json& v, const std::string& prefix = "") {
  if (v.is_object()) {
    for (const auto& [k, child] : v.items()) {
      std::string name = prefix;
      if (!name.empty()) {
        name += ".";
      }
      name += k;
      print_flat(child, name);
    }
    return;
  }
  std::cout << prefix << " = " << text(v) << "\n";
}

int run(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  const char* env = std::getenv("GWCTL_SOCKET");  // NOLINT(concurrency-mt-unsafe)
  std::string socket = env != nullptr ? env : kDefaultSocket;
  bool json = false;
  std::optional<long long> lease;
  std::vector<std::string> rest;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--socket" && i + 1 < args.size()) {
      socket = args[++i];
    } else if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--lease" && i + 1 < args.size()) {
      lease = std::strtoll(args[++i].c_str(), nullptr, 10);
    } else if (args[i] == "-h" || args[i] == "--help") {
      usage();
      return kExitOk;
    } else {
      rest.push_back(args[i]);
    }
  }
  if (rest.empty()) {
    usage();
    return kExitUsage;
  }
  const auto& cmd = rest[0];
  Json request{{"cmd", cmd}};
  if (cmd == "cmd") {
    if (rest.size() < 3) {
      usage();
      return kExitUsage;
    }
    Json a{{"target", rest[1]}, {"op", rest[2]}};
    Json cmd_args = Json::object();
    for (std::size_t i = 3; i < rest.size(); ++i) {
      const auto eq = rest[i].find('=');
      if (eq == std::string::npos) {
        usage();
        return kExitUsage;
      }
      cmd_args[rest[i].substr(0, eq)] = value_of(rest[i].substr(eq + 1));
    }
    if (!cmd_args.empty()) {
      a["args"] = cmd_args;
    }
    if (lease) {
      a["lease_s"] = *lease;
    }
    request["args"] = a;
  } else if (cmd == "diag") {
    if (rest.size() != 2) {
      usage();
      return kExitUsage;
    }
    request["args"] = Json{{"device", rest[1]}};
  } else if (rest.size() != 1) {
    usage();
    return kExitUsage;
  }
  const auto reply_text = send_request(socket, request.dump());
  if (!reply_text) {
    return kExitUsage;
  }
  const auto reply = Json::parse(*reply_text, nullptr, false);
  if (reply.is_discarded()) {
    std::cerr << "gwctl: 応答が JSON でない: " << *reply_text << "\n";
    return kExitError;
  }
  if (json) {
    std::cout << reply.dump() << "\n";
    return reply.value("ok", false) ? kExitOk : kExitError;
  }
  if (!reply.value("ok", false)) {
    std::cerr << "gwctl: " << reply.value("error", "誤り") << "\n";
    return kExitError;
  }
  const auto& result = reply["result"];
  if (cmd == "devices") {
    print_devices(result);
  } else if (cmd == "rules") {
    print_rules(result);
  } else {
    print_flat(result);
  }
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "gwctl: " << e.what() << "\n";
    return kExitUsage;
  }
}
