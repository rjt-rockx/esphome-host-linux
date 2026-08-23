#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include <array>
#include <vector>

#include "esphome/components/api/api_connection.h"
#include "esphome/components/api/api_pb2.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#include "bluetooth_connection.h"

#ifndef BLUETOOTH_PROXY_MAX_CONNECTIONS
#define BLUETOOTH_PROXY_MAX_CONNECTIONS 3
#endif
#ifndef BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE
#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE 16
#endif

namespace esphome {
namespace bluetooth_proxy {

static constexpr int DONE_SENDING_SERVICES = -2;
static constexpr int INIT_SENDING_SERVICES = -3;

using namespace esp32_ble_client;

static constexpr uint32_t LEGACY_ACTIVE_CONNECTIONS_VERSION = 5;
static constexpr uint32_t LEGACY_PASSIVE_ONLY_VERSION = 1;

enum BluetoothProxyFeature : uint32_t {
  FEATURE_PASSIVE_SCAN = 1 << 0,
  FEATURE_ACTIVE_CONNECTIONS = 1 << 1,
  FEATURE_REMOTE_CACHING = 1 << 2,
  FEATURE_PAIRING = 1 << 3,
  FEATURE_CACHE_CLEARING = 1 << 4,
  FEATURE_RAW_ADVERTISEMENTS = 1 << 5,
  FEATURE_STATE_AND_MODE = 1 << 6,
  FEATURE_CONNECTION_PARAMS_SETTING = 1 << 7,
};

enum BluetoothProxySubscriptionFlag : uint32_t {
  SUBSCRIPTION_RAW_ADVERTISEMENTS = 1 << 0,
};

class BluetoothProxy final : public esp32_ble_tracker::ESPBTDeviceListener,
                             public esp32_ble_tracker::BLEScannerStateListener,
                             public Component {
  friend class BluetoothConnection;

 public:
  BluetoothProxy();
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;
  void on_scanner_state(esp32_ble_tracker::ScannerState state) override;
  void dump_config() override;
  void setup() override;
  void loop() override;

  void register_connection(BluetoothConnection *connection) {
    if (this->connection_count_ < BLUETOOTH_PROXY_MAX_CONNECTIONS) {
      this->connections_[this->connection_count_++] = connection;
      connection->proxy_ = this;
    }
  }

  void bluetooth_device_request(const api::BluetoothDeviceRequest &msg);
  void bluetooth_gatt_read(const api::BluetoothGATTReadRequest &msg);
  void bluetooth_gatt_write(const api::BluetoothGATTWriteRequest &msg);
  void bluetooth_gatt_read_descriptor(const api::BluetoothGATTReadDescriptorRequest &msg);
  void bluetooth_gatt_write_descriptor(const api::BluetoothGATTWriteDescriptorRequest &msg);
  void bluetooth_gatt_send_services(const api::BluetoothGATTGetServicesRequest &msg);
  void bluetooth_gatt_notify(const api::BluetoothGATTNotifyRequest &msg);
  void bluetooth_set_connection_params(const api::BluetoothSetConnectionParamsRequest &msg);
  void bluetooth_scanner_set_mode(bool active);

  void subscribe_api_connection(api::APIConnection *api_connection, uint32_t flags);
  void unsubscribe_api_connection(api::APIConnection *api_connection);
  api::APIConnection *get_api_connection() { return this->api_connection_; }

  void send_device_connection(uint64_t address, bool connected, uint16_t mtu = 0, esp_err_t error = ESP_OK);
  void send_connections_free();
  void send_connections_free(api::APIConnection *api_connection);
  void send_gatt_services_done(uint64_t address);
  void send_gatt_error(uint64_t address, uint16_t handle, esp_err_t error);
  void send_device_pairing(uint64_t address, bool paired, esp_err_t error = ESP_OK);
  void send_device_unpairing(uint64_t address, bool success, esp_err_t error = ESP_OK);
  void send_device_clear_cache(uint64_t address, bool success, esp_err_t error = ESP_OK);

  void set_active(bool active) { this->active_ = active; }
  bool has_active() { return this->active_; }

  uint32_t get_legacy_version() const { return this->active_ ? LEGACY_ACTIVE_CONNECTIONS_VERSION : LEGACY_PASSIVE_ONLY_VERSION; }

  uint32_t get_feature_flags() const {
    uint32_t flags = 0;
    flags |= BluetoothProxyFeature::FEATURE_PASSIVE_SCAN;
    flags |= BluetoothProxyFeature::FEATURE_RAW_ADVERTISEMENTS;
    flags |= BluetoothProxyFeature::FEATURE_STATE_AND_MODE;
    if (this->active_) {
      flags |= BluetoothProxyFeature::FEATURE_ACTIVE_CONNECTIONS;
      flags |= BluetoothProxyFeature::FEATURE_REMOTE_CACHING;
      flags |= BluetoothProxyFeature::FEATURE_PAIRING;
      flags |= BluetoothProxyFeature::FEATURE_CACHE_CLEARING;
      flags |= BluetoothProxyFeature::FEATURE_CONNECTION_PARAMS_SETTING;
    }
    return flags;
  }

  // The adapter MAC isn't readily exposed without a BlueZ query; HA only uses
  // this cosmetically, so report empty.
  void get_bluetooth_mac_address_pretty(std::span<char, 18> output) { output[0] = '\0'; }

 protected:
  void send_bluetooth_scanner_state_(esp32_ble_tracker::ScannerState state);
  void flush_pending_advertisements_() {
    if (this->response_.advertisements_len == 0)
      return;
    (void) this->api_connection_->send_message(this->response_);
    this->response_.advertisements_len = 0;
  }
  void log_advertisement_flush_();
  BluetoothConnection *get_connection_(uint64_t address, bool reserve);
  void handle_gatt_not_connected_(uint64_t address, uint16_t handle, const char *action, const char *type);
  void log_not_connected_gatt_(const char *action, const char *type);
  void log_connection_request_ignored_(BluetoothConnection *connection, espbt::ClientState state);
  void log_connection_info_(BluetoothConnection *connection, const char *message);

  api::APIConnection *api_connection_{nullptr};
  std::array<BluetoothConnection *, BLUETOOTH_PROXY_MAX_CONNECTIONS> connections_{};
  api::BluetoothLERawAdvertisementsResponse response_;
  uint32_t last_advertisement_flush_time_{0};
  api::BluetoothConnectionsFreeResponse connections_free_response_;
  bool active_{false};
  uint8_t connection_count_{0};
  bool configured_scan_active_{false};
};

extern BluetoothProxy *global_bluetooth_proxy;  // NOLINT

}  // namespace bluetooth_proxy
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
