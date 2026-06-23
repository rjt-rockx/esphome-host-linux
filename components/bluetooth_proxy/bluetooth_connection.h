#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_client/ble_client_base.h"

namespace esphome {
namespace bluetooth_proxy {

namespace espbt = esphome::esp32_ble_tracker;

class BluetoothProxy;

// One active GATT connection slot the proxy manages on behalf of Home Assistant.
// Results are fanned back to the API client via BLEClientBase node-style hooks.
class BluetoothConnection final : public esp32_ble_client::BLEClientBase {
 public:
  void dump_config() override;
  void loop() override;

  // Handle-based wrappers the proxy calls — forward to the BLEClientBase primitives.
  esp_err_t read_characteristic(uint16_t handle) { return BLEClientBase::read_characteristic(handle); }
  esp_err_t write_characteristic(uint16_t handle, const uint8_t *data, size_t length, bool response) {
    return BLEClientBase::write_characteristic(handle, data, length, response);
  }
  esp_err_t read_descriptor(uint16_t handle) { return BLEClientBase::read_descriptor(handle); }
  esp_err_t write_descriptor(uint16_t handle, const uint8_t *data, size_t length, bool response) {
    return BLEClientBase::write_descriptor(handle, data, length, response);
  }
  esp_err_t notify_characteristic(uint16_t handle, bool enable) {
    return BLEClientBase::notify_characteristic(handle, enable);
  }
  // Connection-parameter tuning is a no-op on host (BlueZ manages it).
  esp_err_t update_connection_params(uint16_t /*min*/, uint16_t /*max*/, uint16_t /*lat*/, uint16_t /*to*/) {
    return ESP_OK;
  }

  void set_address(uint64_t address) override;
  size_t service_count() const { return this->services_.size(); }

 protected:
  friend class BluetoothProxy;

  // The proxy connection consumes worker events directly (it is not a
  // BLEClientNode), fanning results to the API client.
  void dispatch_event_(const esp32_ble_client::HostGattEvent &ev) override;
  void on_disconnect_complete(esp_err_t reason) override;
  void send_service_for_discovery_();
  void reset_connection_(esp_err_t reason);
  bool supports_efficient_uuids_() const;
  void update_allocated_slot_(uint64_t find_value, uint64_t set_value);

  BluetoothProxy *proxy_{nullptr};
  int16_t send_service_{-3};  // -3 = INIT, -2 = DONE, >=0 = next service index
  bool seen_services_{false};
};

}  // namespace bluetooth_proxy
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
