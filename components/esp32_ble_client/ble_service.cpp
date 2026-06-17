#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_service.h"

namespace esphome {
namespace esp32_ble_client {

BLEService::~BLEService() { this->release_characteristics(); }

void BLEService::release_characteristics() {
  for (auto *c : this->characteristics)
    delete c;  // NOLINT(cppcoreguidelines-owning-memory)
  this->characteristics.clear();
}

BLECharacteristic *BLEService::get_characteristic(espbt::ESPBTUUID uuid) {
  for (auto *c : this->characteristics)
    if (c->uuid == uuid)
      return c;
  return nullptr;
}

BLECharacteristic *BLEService::get_characteristic(uint16_t uuid) {
  return this->get_characteristic(espbt::ESPBTUUID::from_uint16(uuid));
}

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
