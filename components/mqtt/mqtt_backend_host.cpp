#include "mqtt_backend_host.h"

#if defined(USE_MQTT) && defined(USE_HOST)

#include "esphome/core/log.h"

#include <cstring>
#include <mosquitto.h>

namespace esphome::mqtt {

static const char *const TAG = "mqtt.host";

namespace {

bool g_library_initialised = false;

void ensure_library_initialised() {
  if (g_library_initialised)
    return;
  mosquitto_lib_init();
  g_library_initialised = true;
}

MQTTClientDisconnectReason translate_rc(int rc) {
  switch (rc) {
    case MOSQ_ERR_CONN_REFUSED:
      return MQTTClientDisconnectReason::MQTT_NOT_AUTHORIZED;
    case MOSQ_ERR_NOT_FOUND:
      return MQTTClientDisconnectReason::DNS_RESOLVE_ERROR;
    case MOSQ_ERR_PROTOCOL:
      return MQTTClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION;
    default:
      return MQTTClientDisconnectReason::TCP_DISCONNECTED;
  }
}

}  // namespace

MQTTBackendHost::MQTTBackendHost() { ensure_library_initialised(); }

MQTTBackendHost::~MQTTBackendHost() {
  if (this->mosq_ != nullptr) {
    if (this->loop_started_.load()) {
      mosquitto_loop_stop(this->mosq_, true);
    }
    mosquitto_destroy(this->mosq_);
    this->mosq_ = nullptr;
  }
}

void MQTTBackendHost::connect() {
  std::lock_guard<std::mutex> g(this->mu_);
  if (this->mosq_ != nullptr) {
    if (this->loop_started_.load()) {
      mosquitto_loop_stop(this->mosq_, true);
      this->loop_started_ = false;
    }
    mosquitto_destroy(this->mosq_);
    this->mosq_ = nullptr;
  }

  const char *id = this->client_id_.empty() ? nullptr : this->client_id_.c_str();
  this->mosq_ = mosquitto_new(id, this->clean_session_, this);
  if (this->mosq_ == nullptr) {
    ESP_LOGE(TAG, "mosquitto_new failed: %s", std::strerror(errno));
    if (this->on_disconnect_)
      this->on_disconnect_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return;
  }

  mosquitto_connect_callback_set(this->mosq_, &MQTTBackendHost::s_on_connect);
  mosquitto_disconnect_callback_set(this->mosq_, &MQTTBackendHost::s_on_disconnect);
  mosquitto_subscribe_callback_set(this->mosq_, &MQTTBackendHost::s_on_subscribe);
  mosquitto_unsubscribe_callback_set(this->mosq_, &MQTTBackendHost::s_on_unsubscribe);
  mosquitto_message_callback_set(this->mosq_, &MQTTBackendHost::s_on_message);
  mosquitto_publish_callback_set(this->mosq_, &MQTTBackendHost::s_on_publish);

  if (!this->username_.empty()) {
    mosquitto_username_pw_set(this->mosq_, this->username_.c_str(),
                              this->password_.empty() ? nullptr : this->password_.c_str());
  }

  if (!this->will_topic_.empty()) {
    mosquitto_will_set(this->mosq_, this->will_topic_.c_str(), static_cast<int>(this->will_payload_.size()),
                       this->will_payload_.data(), this->will_qos_, this->will_retain_);
  }

  // Reconnect backoff: 1s initial, max 60s, exponential.
  mosquitto_reconnect_delay_set(this->mosq_, 1, 60, true);

  this->want_disconnect_ = false;
  int rc = mosquitto_connect_async(this->mosq_, this->host_.c_str(), this->port_, this->keep_alive_);
  if (rc != MOSQ_ERR_SUCCESS) {
    ESP_LOGW(TAG, "mosquitto_connect_async(%s:%u) failed: %s", this->host_.c_str(), this->port_,
             mosquitto_strerror(rc));
    if (this->on_disconnect_)
      this->on_disconnect_(translate_rc(rc));
    mosquitto_destroy(this->mosq_);
    this->mosq_ = nullptr;
    return;
  }

  rc = mosquitto_loop_start(this->mosq_);
  if (rc != MOSQ_ERR_SUCCESS) {
    ESP_LOGE(TAG, "mosquitto_loop_start failed: %s", mosquitto_strerror(rc));
    mosquitto_destroy(this->mosq_);
    this->mosq_ = nullptr;
    return;
  }
  this->loop_started_ = true;
}

void MQTTBackendHost::disconnect() {
  std::lock_guard<std::mutex> g(this->mu_);
  this->want_disconnect_ = true;
  if (this->mosq_ == nullptr)
    return;
  mosquitto_disconnect(this->mosq_);
  if (this->loop_started_.load()) {
    mosquitto_loop_stop(this->mosq_, false);
    this->loop_started_ = false;
  }
}

bool MQTTBackendHost::subscribe(const char *topic, uint8_t qos) {
  std::lock_guard<std::mutex> g(this->mu_);
  if (this->mosq_ == nullptr)
    return false;
  int mid = 0;
  int rc = mosquitto_subscribe(this->mosq_, &mid, topic, qos);
  if (rc != MOSQ_ERR_SUCCESS) {
    ESP_LOGW(TAG, "mosquitto_subscribe(%s) failed: %s", topic, mosquitto_strerror(rc));
    return false;
  }
  return true;
}

bool MQTTBackendHost::unsubscribe(const char *topic) {
  std::lock_guard<std::mutex> g(this->mu_);
  if (this->mosq_ == nullptr)
    return false;
  int mid = 0;
  int rc = mosquitto_unsubscribe(this->mosq_, &mid, topic);
  if (rc != MOSQ_ERR_SUCCESS) {
    ESP_LOGW(TAG, "mosquitto_unsubscribe(%s) failed: %s", topic, mosquitto_strerror(rc));
    return false;
  }
  return true;
}

bool MQTTBackendHost::publish(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain) {
  std::lock_guard<std::mutex> g(this->mu_);
  if (this->mosq_ == nullptr)
    return false;
  int mid = 0;
  int rc = mosquitto_publish(this->mosq_, &mid, topic, static_cast<int>(length), payload, qos, retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    ESP_LOGW(TAG, "mosquitto_publish(%s, qos=%u) failed: %s", topic, qos, mosquitto_strerror(rc));
    return false;
  }
  return true;
}

void MQTTBackendHost::s_on_connect(::mosquitto *, void *userdata, int rc) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  if (rc == 0) {
    self->connected_ = true;
    if (self->on_connect_)
      self->on_connect_(false);
  } else {
    self->connected_ = false;
    if (self->on_disconnect_)
      self->on_disconnect_(MQTTClientDisconnectReason::MQTT_NOT_AUTHORIZED);
  }
}

void MQTTBackendHost::s_on_disconnect(::mosquitto *, void *userdata, int rc) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  self->connected_ = false;
  if (self->on_disconnect_)
    self->on_disconnect_(translate_rc(rc));
}

void MQTTBackendHost::s_on_subscribe(::mosquitto *, void *userdata, int mid, int qos_count, const int *granted_qos) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  uint8_t qos = qos_count > 0 ? static_cast<uint8_t>(granted_qos[0]) : 0;
  if (self->on_subscribe_)
    self->on_subscribe_(static_cast<uint16_t>(mid), qos);
}

void MQTTBackendHost::s_on_unsubscribe(::mosquitto *, void *userdata, int mid) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  if (self->on_unsubscribe_)
    self->on_unsubscribe_(static_cast<uint16_t>(mid));
}

void MQTTBackendHost::s_on_publish(::mosquitto *, void *userdata, int mid) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  if (self->on_publish_)
    self->on_publish_(static_cast<uint16_t>(mid));
}

void MQTTBackendHost::s_on_message(::mosquitto *, void *userdata, const ::mosquitto_message *msg) {
  auto *self = static_cast<MQTTBackendHost *>(userdata);
  if (!self->on_message_ || msg == nullptr)
    return;
  const char *payload = msg->payload ? static_cast<const char *>(msg->payload) : "";
  size_t len = static_cast<size_t>(msg->payloadlen);
  self->on_message_(msg->topic, payload, len, 0, len);
}

}  // namespace esphome::mqtt

#endif  // USE_MQTT && USE_HOST
