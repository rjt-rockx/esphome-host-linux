#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/core/component.h"

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;

class BLEBinaryOutput : public output::BinaryOutput, public BLEClientNode, public Component {
 public:
  void dump_config() override;

  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_characteristic_write(uint16_t handle, int status) override;

  void set_service_uuid16(uint16_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_service_uuid32(uint32_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_service_uuid128(uint8_t *uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_char_uuid16(uint16_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_char_uuid32(uint32_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_char_uuid128(uint8_t *uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_require_response(bool response) { this->require_response_ = response; }

 protected:
  void write_state(bool state) override;
  bool require_response_{false};
  espbt::ESPBTUUID service_uuid_;
  espbt::ESPBTUUID char_uuid_;
  uint16_t char_handle_{0};
  esp_gatt_char_prop_t char_props_{0};
  bool write_with_response_{false};
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
