#pragma once

#include "ble_descriptor.h"

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

namespace esphome {
namespace esp32_ble_server {

// Client Characteristic Configuration Descriptor (CCCD, 0x2902). Inert on host:
// BlueZ owns the CCCD and drives notify subscription via StartNotify/StopNotify,
// so a 0x2902 GattDescriptor1 is never exported. Kept so code that constructs a
// BLE2902 still compiles.
class BLE2902 : public BLEDescriptor {
 public:
  BLE2902();
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
