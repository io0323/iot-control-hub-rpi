// MQTT クライアントの抽象（詳細設計書 第6章、D2）。実装は MosquittoClient と FakeMqttClient。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace gw::adapters {

struct MqttWill {
  std::string topic;
  std::string payload;
  int qos = 1;
  bool retain = true;
};

struct MqttConnectOptions {
  std::string host;
  int port = 1883;
  std::string client_id;
  std::string username;
  std::string password;
  int keepalive_s = 60;
  int max_inflight = 32;    // QoS 1 の確認待ちの上限
  int reconnect_min_s = 1;  // reconnect_delay_set(1, 30, true)
  int reconnect_max_s = 30;
};

struct MqttMessage {
  std::string topic;
  std::string payload;
  bool retained = false;
};

enum class ConnectionEvent : std::uint8_t { kConnected, kDisconnected };

struct PublishResult {
  bool ok = false;
  int mid = 0;
  bool not_connected = false;  // 接続していなかった（呼び出し側は保持する）
  std::string error;
};

class IMqttClient {
 public:
  IMqttClient() = default;
  IMqttClient(const IMqttClient&) = delete;
  IMqttClient& operator=(const IMqttClient&) = delete;
  IMqttClient(IMqttClient&&) = delete;
  IMqttClient& operator=(IMqttClient&&) = delete;
  virtual ~IMqttClient() = default;

  // 非同期。結果は on_connection。失敗しても再接続は続ける
  virtual void connect(const MqttConnectOptions& options, const MqttWill& will) = 0;
  virtual PublishResult publish(std::string_view topic, std::string_view payload, int qos,
                                bool retain) = 0;
  virtual void subscribe(std::string_view filter, int qos) = 0;
  // 正常に切断する（LWT は送られない）。送り途中のパケットは先に送られる
  virtual void disconnect() = 0;
  virtual void on_message(std::function<void(MqttMessage)> handler) = 0;
  virtual void on_connection(std::function<void(ConnectionEvent)> handler) = 0;
  // QoS 1 の PUBACK
  virtual void on_published(std::function<void(int mid)> handler) = 0;
  // 再接続の待ち（バックオフ）を待たずに、今すぐつなぎ直す（ルールの reconnect、open-questions #32）
  virtual void reconnect() = 0;
};

}  // namespace gw::adapters
