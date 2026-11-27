// iot-gw --config <path> [--uart <path>]
//
// 起動（詳細設計書 第4章「起動の手順」）：
//   設定を読んで検証する（誤りは終了コード 2）→ 各スレッドを起動する。
// 終了（第3章、SIGTERM・SIGINT）：
//   受信停止 → core が inbound を空にする → mqtt-tx が gw-01 の offline の state を送り切る → 切断。
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include "adapters/admin/AdminServer.h"
#include "adapters/config/DevicesConfig.h"
#include "adapters/config/GatewayConfig.h"
#include "adapters/config/RequestsConfig.h"
#include "adapters/config/RulesConfig.h"
#include "adapters/history/HistoryStore.h"
#include "adapters/history/HistoryWorker.h"
#include "adapters/mqtt/MosquittoClient.h"
#include "adapters/mqtt/MqttIngest.h"
#include "adapters/mqtt/SchemaValidator.h"
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
constexpr long kSecondsPerMinute = 60;
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

// 実行ファイルの ../share/iot-gw（インストールとビルドで同じ配置）
fs::path default_share_dir(const char* argv0) {
  std::error_code ec;
  auto exe = fs::canonical(argv0, ec);
  if (ec) {
    exe = fs::absolute(argv0, ec);
  }
  return exe.parent_path().parent_path() / "share" / "iot-gw";
}

fs::path default_profiles_dir(const char* argv0) {
  return default_share_dir(argv0) / "profiles";
}

// 統計（open-questions #15：gw-01 の telemetry での公開は共通仕様の変更待ち。それまではログ）
void log_stats(const gw::core::Core& core, const gw::adapters::IngestStats& in,
               const gw::app::MqttTxStats& tx, const gw::app::UartIoStats& uart,
               const gw::app::CoreRunnerStats& runner, bool final) {
  const auto& c = core.stats();
  const auto& cmd = core.commands().stats();
  std::string rejected;
  for (const auto& [reason, count] : in.rejected) {
    rejected += " rejected_" + reason + "=" + std::to_string(count);
  }
  std::string discards;
  for (const auto& [reason, count] : core.bridge().discards()) {
    discards += " uart_discarded_" + reason + "=" + std::to_string(count);
  }
  std::string devices;
  for (const auto& [id, info] : core.registry().devices()) {
    const auto status = core.registry().effective_status(id);
    devices += " device_" + id + "=" +
               (status ? std::string{gw::core::device::to_string(*status)} : std::string{"unseen"});
  }
  const auto avg_us = in.validated == 0
                          ? 0.0
                          : std::chrono::duration<double, std::micro>(in.validate_time).count() /
                                static_cast<double>(in.validated);
  spdlog::info(
      "event=stats final={} mqtt_received={} mqtt_accepted={} own_echo={} requests={} "
      "inbound_dropped={}{} validate_avg_us={:.1f} duplicates={} gaps={} clock_skew={} late={} "
      "unregistered={} stale_transitions={} devices_registered={} device_events={} "
      "uart_frames={}{} published={} dropped_telemetry_rate={} dropped_telemetry_disconnected={} "
      "dropped_telemetry_before_connect={} dropped_held={} uart_disconnects={} "
      "mqtt_out_dropped={} cmd_submitted={} cmd_done={} cmd_rejected={} cmd_failed={} "
      "cmd_timed_out={} cmd_expired={} uart_cmd_sent={} uart_cmd_retries={} mqtt_cmd_sent={} "
      "mqtt_cmd_retries={} request_duplicates={} command_failed_events={} rules={} rules_fired={} "
      "rules_cleared={} rule_commands_rejected={} renewals={} renewals_skipped={} mode={} "
      "mode_changes={} approvals={} reloads={}{}",
      final, in.received, in.accepted, in.own_echo, in.requests, in.inbound_dropped, rejected,
      avg_us, c.mqtt_duplicates, c.mqtt_gaps, c.mqtt_clock_skew, c.mqtt_late, c.mqtt_unregistered,
      c.stale_transitions, c.devices_registered, c.device_events, c.uart_frames, discards,
      tx.published, tx.dropped_telemetry_rate, tx.dropped_telemetry_disconnected,
      tx.dropped_telemetry_before_connect, tx.dropped_held, uart.disconnects, runner.mqtt_dropped,
      cmd.submitted, cmd.done, cmd.rejected, cmd.failed, cmd.timed_out, cmd.expired, cmd.uart_sent,
      cmd.uart_retries, cmd.mqtt_sent, cmd.mqtt_retries, core.requests().stats().duplicates,
      c.command_failed_events, core.rules().rules().size(), c.rules_fired, c.rules_cleared,
      c.rule_commands_rejected, cmd.renewals, cmd.renewals_skipped, core.system_mode().mode(),
      c.mode_changes, core.requests().stats().approvals, c.reloads, devices);
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

// 起動時の復元：未完了の指令を FAILED（restart）で閉じ、直近1時間の requests・台帳・最後のモードを
// core に渡す。履歴DB を読めなければ false（終了コード 2）
bool restore_core(gw::adapters::HistoryStore& store, gw::core::Core& core, std::int64_t now_ms) {
  gw::adapters::RestoreState restored;
  try {
    restored = store.restore(now_ms);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "level=error event=history_restore_failed error=\"%s\"\n",  // NOLINT
                 e.what());
    return false;
  }
  gw::core::CoreRestore r;
  for (const auto& q : restored.requests) {
    r.requests.push_back(gw::core::command::RestoredRequest{
        q.request_id, q.source, q.kind, q.result, q.reason, q.cmd_id, now_ms - q.ts_ms});
  }
  if (restored.last_mode) {
    r.last_mode = restored.last_mode->to_mode;
    r.last_mode_reason = restored.last_mode->reason;
  }
  r.devices = restored.devices;
  core.restore(r);
  spdlog::info("event=history_restored commands_closed={} requests={} devices={} mode={}",
               restored.commands_closed, restored.requests.size(), restored.devices.size(),
               core.system_mode().mode());
  return true;
}

// gwctl の口（admin スレッド）：core への管理要求、再読込、DIAG の読み出し、core の外の統計
gw::adapters::AdminServer::Hooks admin_hooks(BoundedQueue<gw::core::Inbound>& inbound,
                                             std::function<gw::core::Json()> reload,
                                             std::filesystem::path db_path,
                                             const gw::adapters::HistoryWorker& history,
                                             const gw::app::CoreRunner& runner,
                                             const gw::adapters::MqttIngest& ingest) {
  return gw::adapters::AdminServer::Hooks{
      [&inbound](gw::core::AdminRequest r) {
        return inbound.push_for(gw::core::Inbound{std::move(r)}, std::chrono::seconds{1});
      },
      std::move(reload),
      [db = std::move(db_path)](const std::string& device, std::int64_t since_ms) {
        gw::core::Json rows = gw::core::Json::array();
        for (const auto& d : gw::adapters::HistoryStore::read_diags(db, device, since_ms, 1)) {
          rows.push_back(gw::core::Json{{"device_id", d.device_id},
                                        {"boot_id", d.boot_id},
                                        {"received_at", gw::core::format_ts(gw::core::SystemTime{
                                                            gw::core::Millis{d.received_ms}})},
                                        {"uptime_ms", d.uptime_ms},
                                        {"reset_reason", d.reset_reason},
                                        {"reset_count", d.reset_count},
                                        {"rx_crc_errors", d.rx_crc_errors},
                                        {"rx_frame_errors", d.rx_frame_errors},
                                        {"max_loop_us", d.max_loop_us},
                                        {"overruns", d.overruns}});
        }
        return rows;
      },
      [&history, &runner, &ingest] {
        const auto h = history.stats();
        const auto r = runner.stats();
        const auto in = ingest.stats();
        return gw::core::Json{
            {"history", gw::core::Json{{"written", h.written},
                                       {"batches", h.batches},
                                       {"write_failures", h.write_failures},
                                       {"buffered", h.buffered},
                                       {"dropped", h.dropped},
                                       {"purged", h.purged},
                                       {"gaps", h.gaps}}},
            {"queues", gw::core::Json{{"inbound_dropped", in.inbound_dropped},
                                      {"mqtt_out_dropped", r.mqtt_dropped},
                                      {"history_dropped", r.history_dropped},
                                      {"heartbeat_skipped", r.heartbeat_skipped}}}};
      }};
}

// 再読込（SIGHUP と gwctl reload）：rules.yaml・devices.yaml・requests.yaml を読み、すべて検証に
// 通ったときだけ入れ替える。1つでも失敗したら全部旧いまま（FR-07、open-questions #38・#39）
std::function<gw::core::Json()> make_reload(const gw::adapters::GatewayConfig& config,
                                            const std::filesystem::path& profiles,
                                            BoundedQueue<gw::core::Inbound>& inbound) {
  return [&config, profiles, &inbound]() -> gw::core::Json {
    try {
      auto next_devices =
          gw::adapters::load_devices_config(config.devices_file, profiles, config.uart.device_id);
      auto next_requests =
          gw::adapters::load_requests_config(config.requests_file, next_devices.devices);
      auto next_rules = gw::adapters::load_rules_config(config.rules_file, next_devices.devices);
      gw::core::Json result{{"rules", next_rules.rules.size()},
                            {"devices", next_devices.devices.size()},
                            {"requests", next_requests.allow.size()}};
      spdlog::info("event=reload_requested {}", result.dump());
      if (!inbound.push(gw::core::Inbound{gw::core::Reload{
              std::move(next_rules), std::move(next_devices.devices), std::move(next_requests)}})) {
        return gw::core::Json{{"ok", false}, {"error", "終了中"}};
      }
      return gw::core::Json{{"ok", true}, {"result", result}};
    } catch (const std::exception& e) {
      spdlog::error("event=reload_failed kept=all error=\"{}\"", e.what());
      return gw::core::Json{{"ok", false}, {"error", e.what()}};
    }
  };
}

int run(int argc, char** argv) {
  const auto args = parse_args(argc, argv);
  if (!args) {
    usage();
    return kExitUsage;
  }
  gw::adapters::GatewayConfig config;
  gw::core::uart::Profile uart_profile;
  gw::core::uart::Profile self_profile;
  gw::adapters::DevicesConfig devices;
  gw::core::command::RequestRules requests;
  gw::core::rules::RuleSet rules;
  std::filesystem::path profiles;
  std::unique_ptr<gw::adapters::SchemaValidator> validator;
  std::unique_ptr<gw::adapters::HistoryStore> store;
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
    profiles = config.profiles_dir.value_or(default_profiles_dir(argv[0]));
    devices =
        gw::adapters::load_devices_config(config.devices_file, profiles, config.uart.device_id);
    uart_profile = devices.uart_device(config.uart.device_id);
    requests = gw::adapters::load_requests_config(config.requests_file, devices.devices);
    rules = gw::adapters::load_rules_config(config.rules_file, devices.devices);
    self_profile =
        gw::core::uart::load_profile(profiles, std::string{gw::core::constants::kDeviceGw01});
    validator = std::make_unique<gw::adapters::SchemaValidator>(
        config.schemas_dir.value_or(default_share_dir(argv[0]) / "schemas"));
    // 履歴DB を開き、移行を適用する。開けない・移行できないなら終了コード 2（open-questions #48）
    store = std::make_unique<gw::adapters::HistoryStore>(config.history.db_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "level=error event=config_invalid error=\"%s\"\n", e.what());  // NOLINT
    return kExitUsage;
  }
  setup_logging(config.log_level);

  gw::adapters::SignalWatcher::block_signals();
  StopSignal stop;
  // ルールの時刻帯は gateway.yaml の timezone で判定する（open-questions #37）
  ::setenv("TZ", config.timezone.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)：スレッドを作る前
  ::tzset();

  gw::core::SystemClock clock;
  gw::core::MonotonicUlidGenerator ids(clock);
  const auto boot_id = gw::core::format_boot_id(gw::core::random_boot_id());
  gw::core::Core core(
      gw::core::CoreConfig{config.site, std::string{gw::core::constants::kDeviceGw01}, self_profile,
                           uart_profile, std::string{gw::kVersion}, boot_id, devices.devices,
                           requests, rules},
      clock, ids);

  // 起動時の復元（詳細設計書 第4章「起動の手順」、D5）
  if (!restore_core(*store, core, clock.system_now().time_since_epoch().count())) {
    return kExitUsage;
  }

  BoundedQueue<gw::core::Inbound> inbound(config.queues.inbound);
  BoundedQueue<gw::core::UartTx> uart_out(config.queues.uart_out);
  BoundedQueue<gw::core::MqttTx> mqtt_out(config.queues.mqtt_out);
  BoundedQueue<gw::core::history::HistoryRecord> history_queue(config.queues.history);

  gw::adapters::MosquittoClient client;
  gw::app::MqttTxWorker mqtt_tx(client, mqtt_out, clock, config.queues.mqtt_hold);
  // 自分（gw-01）と bridged のデバイス（UART）の電文は購読で戻ってくるので捨てる
  std::set<std::string> own_devices{std::string{gw::core::constants::kDeviceGw01}};
  for (const auto& d : devices.devices) {
    if (d.transport == "uart") {
      own_devices.insert(d.device_id);
    }
  }
  gw::adapters::MqttIngest ingest(*validator, own_devices, clock, [&](gw::core::Inbound msg) {
    return inbound.push_for(std::move(msg), gw::adapters::MqttIngest::kInboundWait);
  });
  client.on_message([&](const gw::adapters::MqttMessage& message) { ingest.handle(message); });
  client.on_connection([&](gw::adapters::ConnectionEvent event) {
    if (event == gw::adapters::ConnectionEvent::kConnected) {
      for (const auto& topic : gw::adapters::MqttIngest::subscriptions(config.site)) {
        client.subscribe(topic, 1);  // clean session なので接続のたびに購読し直す
      }
    }
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
  // 品質の件数（v0.2.3 #45）のうち core の外で数えるもの。core のスレッドから60秒ごとに呼ばれる
  core.set_external_counts([&] {
    const auto in = ingest.stats();
    const auto tx = mqtt_tx.stats();
    std::uint64_t rejected = 0;
    for (const auto& [reason, count] : in.rejected) {
      rejected += count;
    }
    // キュー（inbound・mqtt-out）と切断中の保持で捨てた電文。送信制限（レート）で捨てたものは含めない
    return gw::core::ExternalCounts{rejected, in.inbound_dropped + runner.stats().mqtt_dropped +
                                                  tx.dropped_telemetry_disconnected +
                                                  tx.dropped_telemetry_before_connect +
                                                  tx.dropped_held};
  });
  runner.on_reconnect([&](bool uart, bool mqtt) {
    if (uart) {
      uart_io.reconnect();
    }
    if (mqtt) {
      client.reconnect();
    }
  });
  // 再読込（SIGHUP と gwctl reload）：rules.yaml・devices.yaml・requests.yaml を読み、すべて検証に
  // 通ったときだけ入れ替える。1つでも失敗したら全部旧いまま（FR-07、open-questions #38・#39）
  const auto reload = make_reload(config, profiles, inbound);
  gw::adapters::SignalWatcher signals([&] { stop.notify(); }, [&] { (void)reload(); });

  // 履歴（history スレッド）：100件・200ms の一括、失敗時の1万件の保持、3時の削除
  const auto utc_offset = [](gw::core::SystemTime at) {
    const std::time_t seconds =
        std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
    std::tm local{};
    ::localtime_r(&seconds, &local);
    return static_cast<int>(local.tm_gmtoff / kSecondsPerMinute);
  };
  gw::adapters::HistoryWorker history(
      *store, history_queue, clock, utc_offset,
      [&](std::uint64_t dropped) {
        (void)inbound.try_push(gw::core::Inbound{gw::core::HistoryDropped{dropped, "history"}});
      },
      core.system_mode().mode());
  runner.set_history(&history_queue);

  // gwctl（admin スレッド）。ソケットを作れなければログに出して gwctl なしで動く（#48）
  gw::adapters::AdminServer admin(
      config.admin.socket,
      admin_hooks(inbound, reload, config.history.db_path, history, runner, ingest));
  runner.on_admin_reply([&](const gw::core::AdminReply& reply) { admin.deliver(reply); });
  if (admin.start()) {
    spdlog::info("event=admin_listening socket={}", config.admin.socket.string());
  } else {
    spdlog::error("event=admin_unavailable error=\"{}\"", admin.error());
  }
  runner.on_stats([&](const gw::core::Core& c, bool final) {
    log_stats(c, ingest.stats(), mqtt_tx.stats(), uart_io.stats(), runner.stats(), final);
  });

  spdlog::info("event=start version={} boot_id={} site={} uart={} mqtt={}:{}", gw::kVersion,
               boot_id, config.site, config.uart.path, config.mqtt.host, config.mqtt.port);
  core.set_utc_offset(utc_offset);
  std::thread history_thread([&] { history.run(); });
  std::thread mqtt_tx_thread([&] { mqtt_tx.run(); });
  std::thread core_thread([&] { runner.run(); });
  client.connect(options, gw::adapters::MqttWill{will.topic, will.payload, will.qos, true});
  uart_io.start();

  stop.wait();
  spdlog::info("event=stopping");
  admin.stop();
  uart_io.stop();   // 受信停止
  inbound.close();  // core が inbound を空にして offline の state を出す
  core_thread.join();
  history_thread.join();  // 履歴を書き切る（第3章「終了」）
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
