#ifdef USE_HOST

#include "ble_2902.h"
#include "esphome/components/esp32_ble/ble_uuid.h"

namespace esphome {
namespace esp32_ble_server {

BLE2902::BLE2902() : BLEDescriptor(esp32_ble::ESPBTUUID::from_uint16(0x2902), 2, true, true) {
  this->set_value({0, 0});
}

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
