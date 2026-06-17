#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_client/host_gatt_event.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;
using esp32_ble_client::BLEReadResult;
using esp32_ble_client::BLENotifyEvent;

// Native-host port of BLETextSensor: read/notify a characteristic and publish
// its value as a hex string (matching upstream's default behavior).
class BLETextSensor : public text_sensor::TextSensor, public PollingComponent, public BLEClientNode {
 public:
  void update() override;
  void dump_config() override;

  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_characteristic_read(const BLEReadResult &r) override;
  void on_notify(const BLENotifyEvent &e) override;

  void set_service_uuid16(uint16_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_service_uuid32(uint32_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_service_uuid128(uint8_t *uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_char_uuid16(uint16_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_char_uuid32(uint32_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_char_uuid128(uint8_t *uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_descr_uuid16(uint16_t uuid) { this->descr_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_descr_uuid128(uint8_t *uuid) { this->descr_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_enable_notify(bool notify) { this->notify_ = notify; }
  uint16_t handle{0};

 protected:
  std::string parse_data_(const uint8_t *value, uint16_t value_len);
  bool notify_{false};
  espbt::ESPBTUUID service_uuid_;
  espbt::ESPBTUUID char_uuid_;
  espbt::ESPBTUUID descr_uuid_;
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
