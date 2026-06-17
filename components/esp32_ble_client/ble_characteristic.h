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

// Host BLECharacteristic. Keeps upstream's public field/method shape so stock
// consumers compile, and adds native host read/notify entry points that route
// to BlueZ via the owning BLEClientBase / BLEGattHost (no IDF event model).
class BLECharacteristic {
 public:
  bool parsed = false;
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
  esp_gatt_char_prop_t properties{0};
  std::vector<BLEDescriptor *> descriptors;
  BLEService *service{nullptr};

  ~BLECharacteristic();

  void parse_descriptors() {}  // tree is built eagerly by the worker; no-op
  void release_descriptors();
  BLEDescriptor *get_descriptor(espbt::ESPBTUUID uuid);
  BLEDescriptor *get_descriptor(uint16_t uuid);
  BLEDescriptor *get_descriptor_by_handle(uint16_t handle);

  // Upstream-compatible writes (return esp_err_t). Route to the client.
  esp_err_t write_value(uint8_t *new_val, int16_t new_val_size);
  esp_err_t write_value(uint8_t *new_val, int16_t new_val_size, esp_gatt_write_type_t write_type);

  // Native host read/notify. The callback fires on the main thread from the
  // client's loop() drain; BLEReadResult/BLENotifyEvent data are valid only for
  // the duration of the call (mirror IDF param->read.value lifetime).
  esp_err_t read_value(std::function<void(const BLEReadResult &)> &&cb);
  esp_err_t start_notify(std::function<void(const BLENotifyEvent &)> &&cb);
  esp_err_t stop_notify();
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
