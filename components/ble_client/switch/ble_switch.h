#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;

// Switch reflecting / controlling the BLE client connection (enabled state).
class BLEClientSwitch : public switch_::Switch, public Component, public BLEClientNode {
 public:
  void dump_config() override;
  void on_services_discovered() override;
  void on_disconnected(int reason) override;

 protected:
  void write_state(bool state) override;
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
