#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "ble_characteristic.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace esp32_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

class BLEClientBase;

// Host BLEService — same public shape as upstream. The characteristic tree is
// built eagerly by the worker after ServicesResolved, so parse_characteristics
// is a no-op.
class BLEService {
 public:
  bool parsed = false;
  espbt::ESPBTUUID uuid;
  uint16_t start_handle{0};
  uint16_t end_handle{0};
  std::vector<BLECharacteristic *> characteristics;
  BLEClientBase *client{nullptr};

  ~BLEService();

  void parse_characteristics() {}  // tree built eagerly by the worker; no-op
  void release_characteristics();
  BLECharacteristic *get_characteristic(espbt::ESPBTUUID uuid);
  BLECharacteristic *get_characteristic(uint16_t uuid);
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
