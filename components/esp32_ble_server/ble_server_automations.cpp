#ifdef USE_HOST

#include "ble_server_automations.h"

namespace esphome {
namespace esp32_ble_server {
namespace esp32_ble_server_automations {

using namespace esp32_ble;

#ifdef USE_ESP32_BLE_SERVER_CHARACTERISTIC_ON_WRITE
Trigger<std::vector<uint8_t>, uint16_t> *BLETriggers::create_characteristic_on_write_trigger(
    BLECharacteristic *characteristic) {
  Trigger<std::vector<uint8_t>, uint16_t> *on_write_trigger =  // NOLINT(cppcoreguidelines-owning-memory)
      new Trigger<std::vector<uint8_t>, uint16_t>();
  characteristic->on_write([on_write_trigger](std::span<const uint8_t> data, uint16_t id) {
    on_write_trigger->trigger(std::vector<uint8_t>(data.begin(), data.end()), id);
  });
  return on_write_trigger;
}
#endif

#ifdef USE_ESP32_BLE_SERVER_DESCRIPTOR_ON_WRITE
Trigger<std::vector<uint8_t>, uint16_t> *BLETriggers::create_descriptor_on_write_trigger(BLEDescriptor *descriptor) {
  Trigger<std::vector<uint8_t>, uint16_t> *on_write_trigger =  // NOLINT(cppcoreguidelines-owning-memory)
      new Trigger<std::vector<uint8_t>, uint16_t>();
  descriptor->on_write([on_write_trigger](std::span<const uint8_t> data, uint16_t id) {
    on_write_trigger->trigger(std::vector<uint8_t>(data.begin(), data.end()), id);
  });
  return on_write_trigger;
}
#endif

#ifdef USE_ESP32_BLE_SERVER_ON_CONNECT
Trigger<uint16_t> *BLETriggers::create_server_on_connect_trigger(BLEServer *server) {
  Trigger<uint16_t> *on_connect_trigger = new Trigger<uint16_t>();  // NOLINT(cppcoreguidelines-owning-memory)
  server->on_connect([on_connect_trigger](uint16_t conn_id) { on_connect_trigger->trigger(conn_id); });
  return on_connect_trigger;
}
#endif

#ifdef USE_ESP32_BLE_SERVER_ON_DISCONNECT
Trigger<uint16_t> *BLETriggers::create_server_on_disconnect_trigger(BLEServer *server) {
  Trigger<uint16_t> *on_disconnect_trigger = new Trigger<uint16_t>();  // NOLINT(cppcoreguidelines-owning-memory)
  server->on_disconnect([on_disconnect_trigger](uint16_t conn_id) { on_disconnect_trigger->trigger(conn_id); });
  return on_disconnect_trigger;
}
#endif

#ifdef USE_ESP32_BLE_SERVER_SET_VALUE_ACTION
void BLECharacteristicSetValueActionManager::set_listener(BLECharacteristic *characteristic,
                                                          const std::function<void()> &pre_notify_listener) {
  if (this->find_listener_(characteristic) != nullptr) {
    this->remove_listener_(characteristic);
  }
  this->listeners_.push_back({characteristic, pre_notify_listener});
}

BLECharacteristicSetValueActionManager::ListenerEntry *BLECharacteristicSetValueActionManager::find_listener_(
    BLECharacteristic *characteristic) {
  for (auto &entry : this->listeners_) {
    if (entry.characteristic == characteristic) {
      return &entry;
    }
  }
  return nullptr;
}

void BLECharacteristicSetValueActionManager::remove_listener_(BLECharacteristic *characteristic) {
  for (size_t i = 0; i < this->listeners_.size(); i++) {
    if (this->listeners_[i].characteristic == characteristic) {
      this->listeners_[i] = this->listeners_.back();
      this->listeners_.pop_back();
      return;
    }
  }
}
#endif

}  // namespace esp32_ble_server_automations
}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
