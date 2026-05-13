#pragma once

#include "esphome/core/defines.h"

#if defined(USE_MQTT) && defined(USE_HOST)

#include "mqtt_backend.h"
#include "esphome/components/network/ip_address.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

// libmosquitto opaque handles. Declared at global namespace so the static
// callback signatures match the C library's exact prototypes (otherwise the
// compiler treats `struct mosquitto_message` as `esphome::mqtt::mosquitto_message`).
struct mosquitto;
struct mosquitto_message;

namespace esphome::mqtt {

// Host MQTT backend backed by libmosquitto. The library handles framing,
// reconnect, keepalive, and a worker thread internally; we forward its
// callbacks back into the ESPHome callback signatures defined on MQTTBackend.
class MQTTBackendHost final : public MQTTBackend {
 public:
  MQTTBackendHost();
  ~MQTTBackendHost();

  void set_keep_alive(uint16_t keep_alive) override { this->keep_alive_ = keep_alive; }
  void set_client_id(const char *client_id) override { this->client_id_ = client_id ? client_id : ""; }
  void set_clean_session(bool clean_session) override { this->clean_session_ = clean_session; }
  void set_credentials(const char *username, const char *password) override {
    this->username_ = username ? username : "";
    this->password_ = password ? password : "";
  }
  void set_will(const char *topic, uint8_t qos, bool retain, const char *payload) override {
    this->will_topic_ = topic ? topic : "";
    this->will_payload_ = payload ? payload : "";
    this->will_qos_ = qos;
    this->will_retain_ = retain;
  }
  void set_server(network::IPAddress ip, uint16_t port) override {
    char buf[network::IP_ADDRESS_BUFFER_SIZE];
    this->host_ = ip.str_to(buf);
    this->port_ = port;
  }
  void set_server(const char *host, uint16_t port) override {
    this->host_ = host ? host : "";
    this->port_ = port;
  }

  void set_on_connect(std::function<on_connect_callback_t> &&cb) override { this->on_connect_ = std::move(cb); }
  void set_on_disconnect(std::function<on_disconnect_callback_t> &&cb) override {
    this->on_disconnect_ = std::move(cb);
  }
  void set_on_subscribe(std::function<on_subscribe_callback_t> &&cb) override { this->on_subscribe_ = std::move(cb); }
  void set_on_unsubscribe(std::function<on_unsubscribe_callback_t> &&cb) override {
    this->on_unsubscribe_ = std::move(cb);
  }
  void set_on_message(std::function<on_message_callback_t> &&cb) override { this->on_message_ = std::move(cb); }
  void set_on_publish(std::function<on_publish_user_callback_t> &&cb) override { this->on_publish_ = std::move(cb); }

  bool connected() const override { return this->connected_.load(); }
  void connect() override;
  void disconnect() override;

  bool subscribe(const char *topic, uint8_t qos) override;
  bool unsubscribe(const char *topic) override;
  bool publish(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain) override;

  // No-op TLS hooks. libmosquitto supports TLS via mosquitto_tls_set; left
  // unimplemented for now since the most common Pi MQTT deployments use a
  // local broker on localhost or a LAN broker without TLS.
  void set_ca_certificate(const char *cert) {
    if (cert)
      this->ca_certificate_ = cert;
  }
  void set_cl_certificate(const char *cert) {
    if (cert)
      this->cl_certificate_ = cert;
  }
  void set_cl_key(const char *key) {
    if (key)
      this->cl_key_ = key;
  }
  void set_skip_cert_cn_check(bool skip) { this->skip_cert_cn_check_ = skip; }

 protected:
  static void s_on_connect(::mosquitto *m, void *userdata, int rc);
  static void s_on_disconnect(::mosquitto *m, void *userdata, int rc);
  static void s_on_subscribe(::mosquitto *m, void *userdata, int mid, int qos_count, const int *granted_qos);
  static void s_on_unsubscribe(::mosquitto *m, void *userdata, int mid);
  static void s_on_publish(::mosquitto *m, void *userdata, int mid);
  static void s_on_message(::mosquitto *m, void *userdata, const ::mosquitto_message *msg);

  ::mosquitto *mosq_{nullptr};
  std::atomic<bool> connected_{false};
  std::atomic<bool> want_disconnect_{false};
  std::atomic<bool> loop_started_{false};
  std::mutex mu_;

  uint16_t keep_alive_{15};
  std::string client_id_;
  bool clean_session_{true};
  std::string username_;
  std::string password_;
  std::string will_topic_;
  std::string will_payload_;
  uint8_t will_qos_{0};
  bool will_retain_{false};
  std::string host_{"127.0.0.1"};
  uint16_t port_{1883};

  std::string ca_certificate_;
  std::string cl_certificate_;
  std::string cl_key_;
  bool skip_cert_cn_check_{false};

  std::function<on_connect_callback_t> on_connect_;
  std::function<on_disconnect_callback_t> on_disconnect_;
  std::function<on_subscribe_callback_t> on_subscribe_;
  std::function<on_unsubscribe_callback_t> on_unsubscribe_;
  std::function<on_message_callback_t> on_message_;
  std::function<on_publish_user_callback_t> on_publish_;
};

}  // namespace esphome::mqtt

#endif  // USE_MQTT && USE_HOST
