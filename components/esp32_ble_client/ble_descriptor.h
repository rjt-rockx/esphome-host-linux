#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include <cstdint>

namespace esphome {
namespace esp32_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

class BLECharacteristic;

// A discovered GATT descriptor.
class BLEDescriptor {
 public:
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
  BLECharacteristic *characteristic{nullptr};
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
