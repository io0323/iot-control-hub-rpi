// libmosquitto による IMqttClient（D2）。mosquitto_loop_start
// のスレッド（mqtt-loop）でコールバックを呼ぶ。
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "adapters/mqtt/IMqttClient.h"

struct mosquitto;

namespace gw::adapters {

class MosquittoClient final : public IMqttClient {
 public:
  MosquittoClient();
  ~MosquittoClient() override;
  MosquittoClient(const MosquittoClient&) = delete;
  MosquittoClient& operator=(const MosquittoClient&) = delete;
  MosquittoClient(MosquittoClient&&) = delete;
  MosquittoClient& operator=(MosquittoClient&&) = delete;

  void connect(const MqttConnectOptions& options, const MqttWill& will) override;
  PublishResult publish(std::string_view topic, std::string_view payload, int qos,
                        bool retain) override;
  void subscribe(std::string_view filter, int qos) override;
  void disconnect() override;
  void on_message(std::function<void(MqttMessage)> handler) override;
  void on_connection(std::function<void(ConnectionEvent)> handler) override;
  void on_published(std::function<void(int mid)> handler) override;
  void reconnect() override;

  // mqtt-loop のスレッドを止める（disconnect の後に呼ぶ）
  void stop_loop();

  // libmosquitto のコールバックから呼ぶ（mqtt-loop のスレッド）
  void handle_connection(ConnectionEvent event);
  void handle_published(int mid);
  void handle_message(MqttMessage message);

 private:
  mosquitto* mosq_ = nullptr;
  std::mutex handlers_mutex_;
  std::function<void(MqttMessage)> message_handler_;
  std::function<void(ConnectionEvent)> connection_handler_;
  std::function<void(int)> published_handler_;
  std::atomic<bool> loop_started_{false};
};

}  // namespace gw::adapters
