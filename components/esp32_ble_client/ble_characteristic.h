#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "ble_descriptor.h"
#include "host_gatt_event.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace esphome {
namespace esp32_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

class BLEService;

// A discovered GATT characteristic. Read/write/notify entry points route to
// BlueZ via the owning BLEService / BLEClientBase.
class BLECharacteristic {
 public:
  bool parsed = false;
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
  esp_gatt_char_prop_t properties{0};
  std::vector<BLEDescriptor *> descriptors;
  BLEService *service{nullptr};

  ~BLECharacteristic();

  void parse_descriptors() {}  // no-op: descriptor tree is built eagerly at discovery
  void release_descriptors();
  BLEDescriptor *get_descriptor(espbt::ESPBTUUID uuid);
  BLEDescriptor *get_descriptor(uint16_t uuid);
  BLEDescriptor *get_descriptor_by_handle(uint16_t handle);

  esp_err_t write_value(uint8_t *new_val, int16_t new_val_size);
  esp_err_t write_value(uint8_t *new_val, int16_t new_val_size, esp_gatt_write_type_t write_type);

  // Read/notify. The callback fires on the main thread from the client's loop()
  // drain; BLEReadResult/BLENotifyEvent data are valid only for the duration of
  // the call.
  esp_err_t read_value(std::function<void(const BLEReadResult &)> &&cb);
  esp_err_t start_notify(std::function<void(const BLENotifyEvent &)> &&cb);
  esp_err_t stop_notify();
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
