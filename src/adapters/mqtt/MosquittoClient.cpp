#include "adapters/mqtt/MosquittoClient.h"

#include <stdexcept>

#include <mosquitto.h>

namespace gw::adapters {

namespace {

std::once_flag lib_init_once;

MosquittoClient* self_of(void* userdata) {
  return static_cast<MosquittoClient*>(userdata);
}

void cb_connect(mosquitto* /*mosq*/, void* userdata, int rc) {
  if (rc == 0) {
    self_of(userdata)->handle_connection(ConnectionEvent::kConnected);
  }
}

void cb_disconnect(mosquitto* /*mosq*/, void* userdata, int /*rc*/) {
  self_of(userdata)->handle_connection(ConnectionEvent::kDisconnected);
}

void cb_publish(mosquitto* /*mosq*/, void* userdata, int mid) {
  self_of(userdata)->handle_published(mid);
}

void cb_message(mosquitto* /*mosq*/, void* userdata, const mosquitto_message* msg) {
  MqttMessage message;
  message.topic = msg->topic != nullptr ? msg->topic : "";
  if (msg->payload != nullptr && msg->payloadlen > 0) {
    message.payload.assign(static_cast<const char*>(msg->payload),
                           static_cast<std::size_t>(msg->payloadlen));
  }
  message.retained = msg->retain;
  self_of(userdata)->handle_message(std::move(message));
}

}  // namespace

MosquittoClient::MosquittoClient() {
  std::call_once(lib_init_once, [] { mosquitto_lib_init(); });
}

MosquittoClient::~MosquittoClient() {
  stop_loop();
  if (mosq_ != nullptr) {
    mosquitto_destroy(mosq_);
  }
}

void MosquittoClient::connect(const MqttConnectOptions& options, const MqttWill& will) {
  if (mosq_ == nullptr) {
    mosq_ = mosquitto_new(options.client_id.c_str(), true, this);
    if (mosq_ == nullptr) {
      throw std::runtime_error("mosquitto_new に失敗した");
    }
    mosquitto_connect_callback_set(mosq_, cb_connect);
    mosquitto_disconnect_callback_set(mosq_, cb_disconnect);
    mosquitto_publish_callback_set(mosq_, cb_publish);
    mosquitto_message_callback_set(mosq_, cb_message);
  }
  mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
  mosquitto_int_option(mosq_, MOSQ_OPT_SEND_MAXIMUM, options.max_inflight);
  mosquitto_max_inflight_messages_set(mosq_, static_cast<unsigned>(options.max_inflight));
  mosquitto_reconnect_delay_set(mosq_, static_cast<unsigned>(options.reconnect_min_s),
                                static_cast<unsigned>(options.reconnect_max_s), true);
  if (!options.username.empty()) {
    mosquitto_username_pw_set(mosq_, options.username.c_str(),
                              options.password.empty() ? nullptr : options.password.c_str());
  }
  mosquitto_will_set(mosq_, will.topic.c_str(), static_cast<int>(will.payload.size()),
                     will.payload.data(), will.qos, will.retain);
  // 失敗してもループのスレッドが再接続を続ける（ブローカーが後から起動してもつながる）
  (void)mosquitto_connect_async(mosq_, options.host.c_str(), options.port, options.keepalive_s);
  if (!loop_started_.exchange(true)) {
    if (mosquitto_loop_start(mosq_) != MOSQ_ERR_SUCCESS) {
      loop_started_ = false;
      throw std::runtime_error("mosquitto_loop_start に失敗した");
    }
  }
}

PublishResult MosquittoClient::publish(std::string_view topic, std::string_view payload, int qos,
                                       bool retain) {
  PublishResult result;
  if (mosq_ == nullptr) {
    result.not_connected = true;
    return result;
  }
  const std::string topic_text{topic};
  const int rc = mosquitto_publish(mosq_, &result.mid, topic_text.c_str(),
                                   static_cast<int>(payload.size()), payload.data(), qos, retain);
  result.ok = rc == MOSQ_ERR_SUCCESS;
  result.not_connected = rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST;
  if (!result.ok) {
    result.error = mosquitto_strerror(rc);
  }
  return result;
}

void MosquittoClient::subscribe(std::string_view filter, int qos) {
  if (mosq_ != nullptr) {
    const std::string text{filter};
    (void)mosquitto_subscribe(mosq_, nullptr, text.c_str(), qos);
  }
}

void MosquittoClient::disconnect() {
  if (mosq_ != nullptr) {
    (void)mosquitto_disconnect(mosq_);
  }
}

void MosquittoClient::stop_loop() {
  if (mosq_ != nullptr && loop_started_.exchange(false)) {
    // disconnect の後なら false（スレッドが終わるのを待つ）、まだなら true（強制終了）
    (void)mosquitto_loop_stop(mosq_, false);
  }
}

void MosquittoClient::on_message(std::function<void(MqttMessage)> handler) {
  const std::lock_guard lock(handlers_mutex_);
  message_handler_ = std::move(handler);
}

void MosquittoClient::on_connection(std::function<void(ConnectionEvent)> handler) {
  const std::lock_guard lock(handlers_mutex_);
  connection_handler_ = std::move(handler);
}

void MosquittoClient::on_published(std::function<void(int)> handler) {
  const std::lock_guard lock(handlers_mutex_);
  published_handler_ = std::move(handler);
}

void MosquittoClient::handle_connection(ConnectionEvent event) {
  std::function<void(ConnectionEvent)> handler;
  {
    const std::lock_guard lock(handlers_mutex_);
    handler = connection_handler_;
  }
  if (handler) {
    handler(event);
  }
}

void MosquittoClient::handle_published(int mid) {
  std::function<void(int)> handler;
  {
    const std::lock_guard lock(handlers_mutex_);
    handler = published_handler_;
  }
  if (handler) {
    handler(mid);
  }
}

void MosquittoClient::handle_message(MqttMessage message) {
  std::function<void(MqttMessage)> handler;
  {
    const std::lock_guard lock(handlers_mutex_);
    handler = message_handler_;
  }
  if (handler) {
    handler(std::move(message));
  }
}

}  // namespace gw::adapters
