// iot-gw --config <path> [--uart <path>]
//
// 起動（詳細設計書 第4章「起動の手順」）：
//   設定を読んで検証する（誤りは終了コード 2）→ 各スレッドを起動する。
// 終了（第3章、SIGTERM・SIGINT）：
//   受信停止 → core が inbound を空にする → mqtt-tx が gw-01 の offline の state を送り切る → 切断。
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include "adapters/config/GatewayConfig.h"
#include "adapters/mqtt/MosquittoClient.h"
#include "adapters/platform/SignalWatcher.h"
#include "app/CoreRunner.h"
#include "app/MqttTxWorker.h"
#include "app/UartIo.h"
#include "core/Core.h"
#include "core/common/BoundedQueue.h"
#include "core/common/Clock.h"
#include "core/common/Constants.h"
#include "core/common/Ids.h"
#include "core/uart/Profile.h"
#include "gw_version.h"

namespace {

namespace fs = std::filesystem;
using gw::core::BoundedQueue;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;  // 設定・引数の誤り（第4章）
constexpr std::chrono::milliseconds kMqttCallbackWait{
    200};  // mqtt-loop が inbound で待つ上限（第3章）

struct Args {
  fs::path config;
  std::optional<std::string> uart;
};

void usage() {
  std::cerr << "使い方: iot-gw --config <gateway.yaml> [--uart <デバイスパス>]\n";
}

std::optional<Args> parse_args(int argc, char** argv) {
  Args args;
  const std::vector<std::string> list(argv + 1, argv + argc);
  for (std::size_t i = 0; i < list.size(); ++i) {
    const auto& a = list[i];
    if ((a == "--config" || a == "--uart") && i + 1 < list.size()) {
      if (a == "--config") {
        args.config = list[++i];
      } else {
        args.uart = list[++i];
      }
    } else if (a == "--help" || a == "-h") {
      return std::nullopt;
    } else {
      std::cerr << "知らない引数: " << a << "\n";
      return std::nullopt;
    }
  }
  if (args.config.empty()) {
    return std::nullopt;
  }
  return args;
}

fs::path default_profiles_dir(const char* argv0) {
  std::error_code ec;
  auto exe = fs::canonical(argv0, ec);
  if (ec) {
    exe = fs::absolute(argv0, ec);
  }
  return exe.parent_path().parent_path() / "share" / "iot-gw" / "profiles";
}

// $CREDENTIALS_DIRECTORY/mqtt_password（systemd）、無ければ IOTSPEC_MQTT_PASSWORD（開発・CI）
std::string mqtt_password() {
  if (const char* dir = std::getenv("CREDENTIALS_DIRECTORY")) {  // NOLINT(concurrency-mt-unsafe)
    const std::ifstream in(fs::path(dir) / "mqtt_password");
    if (in) {
      std::stringstream text;
      text << in.rdbuf();
      auto value = text.str();
      while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
      }
      return value;
    }
  }
  if (const char* env = std::getenv("IOTSPEC_MQTT_PASSWORD")) {  // NOLINT(concurrency-mt-unsafe)
    return env;
  }
  return {};
}

void setup_logging(const std::string& level) {
  auto logger = spdlog::stderr_logger_mt("iot-gw");
  // 標準エラー出力に1行1イベント（key=value）。journald が拾う（第7章）
  logger->set_pattern("%Y-%m-%dT%H:%M:%S.%eZ level=%l %v", spdlog::pattern_time_type::utc);
  logger->set_level(spdlog::level::from_str(level));
  logger->flush_on(spdlog::level::trace);
  spdlog::set_default_logger(logger);
}

class StopSignal {
 public:
  void notify() {
    {
      const std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stopped_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopped_ = false;
};

int run(int argc, char** argv) {
  const auto args = parse_args(argc, argv);
  if (!args) {
    usage();
    return kExitUsage;
  }
  gw::adapters::GatewayConfig config;
  gw::core::uart::Profile uart_profile;
  gw::core::uart::Profile self_profile;
  try {
    config = gw::adapters::load_gateway_config(args->config);
    if (args->uart) {
      config.uart.path = *args->uart;
    }
    if (config.uart.path.empty()) {
      throw gw::adapters::ConfigError(
          args->config.string(),
          {"/uart/path: UART のデバイスパスがない（--uart でも指定できる）"});
    }
    const auto profiles = config.profiles_dir.value_or(default_profiles_dir(argv[0]));
    uart_profile = gw::core::uart::load_profile(profiles, config.uart.device_id);
    self_profile =
        gw::core::uart::load_profile(profiles, std::string{gw::core::constants::kDeviceGw01});
  } catch (const std::exception& e) {
    std::fprintf(stderr, "level=error event=config_invalid error=\"%s\"\n", e.what());  // NOLINT
    return kExitUsage;
  }
  setup_logging(config.log_level);

  gw::adapters::SignalWatcher::block_signals();
  StopSignal stop;
  gw::adapters::SignalWatcher signals([&] { stop.notify(); },
                                      [] { spdlog::info("event=sighup action=ignored phase=1"); });

  gw::core::SystemClock clock;
  gw::core::MonotonicUlidGenerator ids(clock);
  const auto boot_id = gw::core::format_boot_id(gw::core::random_boot_id());
  gw::core::Core core(
      gw::core::CoreConfig{config.site, std::string{gw::core::constants::kDeviceGw01}, self_profile,
                           uart_profile, std::string{gw::kVersion}, boot_id},
      clock, ids);

  BoundedQueue<gw::core::Inbound> inbound(config.queues.inbound);
  BoundedQueue<gw::core::UartTx> uart_out(config.queues.uart_out);
  BoundedQueue<gw::core::MqttTx> mqtt_out(config.queues.mqtt_out);

  gw::adapters::MosquittoClient client;
  gw::app::MqttTxWorker mqtt_tx(client, mqtt_out, clock, config.queues.mqtt_hold);
  client.on_connection([&](gw::adapters::ConnectionEvent event) {
    mqtt_tx.on_connection(event);
    spdlog::info("event=mqtt_{}",
                 event == gw::adapters::ConnectionEvent::kConnected ? "connected" : "disconnected");
    const gw::core::Inbound msg = event == gw::adapters::ConnectionEvent::kConnected
                                      ? gw::core::Inbound{gw::core::MqttConnected{}}
                                      : gw::core::Inbound{gw::core::MqttDisconnected{}};
    if (!inbound.push_for(msg, kMqttCallbackWait) && !inbound.closed()) {
      spdlog::warn("event=inbound_full source=mqtt-loop dropped=connection_event");
    }
  });
  client.on_published([&](int mid) { mqtt_tx.on_published(mid); });

  const auto will = core.gw_offline_state();
  gw::adapters::MqttConnectOptions options;
  options.host = config.mqtt.host;
  options.port = config.mqtt.port;
  options.client_id = std::string{gw::core::constants::kDeviceGw01};
  options.username = std::string{gw::core::constants::kDeviceGw01};
  options.password = mqtt_password();
  options.keepalive_s = config.mqtt.keepalive_s;
  options.max_inflight = gw::core::constants::kMqttMaxInFlight;

  gw::app::UartIo uart_io(config.uart.path, config.uart.baud, inbound, uart_out, clock);
  gw::app::CoreRunner runner(core, clock, inbound, uart_out, mqtt_out, [&] { uart_io.notify(); });

  spdlog::info("event=start version={} boot_id={} site={} uart={} mqtt={}:{}", gw::kVersion,
               boot_id, config.site, config.uart.path, config.mqtt.host, config.mqtt.port);
  std::thread mqtt_tx_thread([&] { mqtt_tx.run(); });
  std::thread core_thread([&] { runner.run(); });
  client.connect(options, gw::adapters::MqttWill{will.topic, will.payload, will.qos, true});
  uart_io.start();

  stop.wait();
  spdlog::info("event=stopping");
  uart_io.stop();   // 受信停止
  inbound.close();  // core が inbound を空にして offline の state を出す
  core_thread.join();
  mqtt_tx_thread.join();  // mqtt-out を送り切る
  client.disconnect();
  client.stop_loop();
  signals.stop();
  spdlog::info("event=stopped");
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "level=critical event=fatal error=\"%s\"\n", e.what());  // NOLINT
    return 1;
  }
}
