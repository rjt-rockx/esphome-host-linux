#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_switch.h"

#include "esphome/core/log.h"

namespace esphome {
namespace ble_client {

static const char *const TAG = "ble_switch";

void BLEClientSwitch::dump_config() { LOG_SWITCH("", "BLE Client Switch", this); }

void BLEClientSwitch::write_state(bool state) {
  this->parent()->set_enabled(state);
  this->publish_state(state);
}

void BLEClientSwitch::on_services_discovered() {
  this->node_state = espbt::ClientState::ESTABLISHED;
  this->publish_state(this->parent()->enabled);
}

void BLEClientSwitch::on_disconnected(int /*reason*/) {
  this->node_state = espbt::ClientState::IDLE;
  this->publish_state(this->parent()->enabled);
}

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
