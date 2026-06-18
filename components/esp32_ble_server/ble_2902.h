#pragma once

#include "ble_descriptor.h"

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

namespace esphome {
namespace esp32_ble_server {

// Client Characteristic Configuration Descriptor (CCCD, 0x2902). On host this is
// inert: BlueZ owns the CCCD and drives notify subscription via StartNotify/
// StopNotify, so BLEGattServer never exports a 0x2902 GattDescriptor1. Kept only
// so anything that still constructs a BLE2902 compiles. Never codegen'd (the
// Python create_notify_cccd path's 0x2902 is filtered at export).
class BLE2902 : public BLEDescriptor {
 public:
  BLE2902();
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
