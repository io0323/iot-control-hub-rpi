// テスト用の IMqttClient。発行した電文を記録し、接続・PUBACK を手で起こせる。
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "adapters/mqtt/IMqttClient.h"

namespace gw::adapters {

struct FakePublished {
  std::string topic;
  std::string payload;
  int qos = 0;
  bool retain = false;
  int mid = 0;
};

class FakeMqttClient final : public IMqttClient {
 public:
  void connect(const MqttConnectOptions& options, const MqttWill& will) override;
  PublishResult publish(std::string_view topic, std::string_view payload, int qos,
                        bool retain) override;
  void subscribe(std::string_view filter, int qos) override;
  void disconnect() override;
  void on_message(std::function<void(MqttMessage)> handler) override;
  void on_connection(std::function<void(ConnectionEvent)> handler) override;
  void on_published(std::function<void(int mid)> handler) override;

  // ---- テストから操作する
  void set_connected(bool connected);  // on_connection を呼ぶ
  void ack(int mid);                   // on_published を呼ぶ
  void ack_all();
  [[nodiscard]] std::vector<FakePublished> published() const;
  [[nodiscard]] std::vector<int> unacked() const;
  [[nodiscard]] std::optional<MqttWill> will() const;
  [[nodiscard]] bool disconnected() const;
  // true なら PUBACK を自動で返す
  void set_auto_ack(bool value);

 private:
  mutable std::mutex mutex_;
  bool connected_ = false;
  bool auto_ack_ = false;
  bool disconnected_ = false;
  int next_mid_ = 1;
  std::optional<MqttWill> will_;
  std::vector<FakePublished> published_;
  std::vector<int> unacked_;
  std::vector<std::string> subscriptions_;
  std::function<void(MqttMessage)> message_handler_;
  std::function<void(ConnectionEvent)> connection_handler_;
  std::function<void(int)> published_handler_;
};

}  // namespace gw::adapters
