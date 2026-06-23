#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_client/host_gatt_event.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

#include <vector>

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;
using esp32_ble_client::BLEReadResult;
using esp32_ble_client::BLENotifyEvent;

// Sensor backed by a GATT characteristic. Polls reads and/or subscribes to
// notifications, parsing the value to a float via the configured lambda or the
// default (first byte) parse.
class BLESensor : public sensor::Sensor, public PollingComponent, public BLEClientNode {
 public:
  void update() override;
  void dump_config() override;

  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_characteristic_read(const BLEReadResult &r) override;
  void on_descriptor_read(const BLEReadResult &r) override;
  void on_notify(const BLENotifyEvent &e) override;

  void set_service_uuid16(uint16_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_service_uuid32(uint32_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_service_uuid128(uint8_t *uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_char_uuid16(uint16_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_char_uuid32(uint32_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_char_uuid128(uint8_t *uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_descr_uuid16(uint16_t uuid) { this->descr_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_descr_uuid32(uint32_t uuid) { this->descr_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_descr_uuid128(uint8_t *uuid) { this->descr_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_data_to_value(float (*lambda)(const std::vector<uint8_t> &)) {
    this->data_to_value_func_ = lambda;
    this->has_data_to_value_ = true;
  }
  void set_enable_notify(bool notify) { this->notify_ = notify; }
  uint16_t handle{0};

 protected:
  float parse_data_(const uint8_t *value, uint16_t value_len);
  bool has_data_to_value_{false};
  float (*data_to_value_func_)(const std::vector<uint8_t> &){};
  bool notify_{false};
  bool handle_is_descriptor_{false};
  espbt::ESPBTUUID service_uuid_;
  espbt::ESPBTUUID char_uuid_;
  espbt::ESPBTUUID descr_uuid_;
};

// RSSI sensor: polls Device1.RSSI of the connected peer.
class BLEClientRSSISensor : public sensor::Sensor, public PollingComponent, public BLEClientNode {
 public:
  void update() override;
  void dump_config() override;
  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_rssi(int8_t rssi) override;
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
