#pragma once

#include "ble_server.h"
#include "ble_characteristic.h"
#include "ble_descriptor.h"

#include "esphome/core/automation.h"

#include <functional>
#include <vector>

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// Host shadow of esp32_ble_server's automations. Pure ESPHome glue (no IDF, no
// D-Bus): ported verbatim from upstream — triggers wire to on_write/on_connect/
// on_disconnect, the set-value action manages a per-characteristic read listener,
// and notify fires the pre-notify listener then notify(). Guarded USE_HOST.

namespace esphome {
namespace esp32_ble_server {
namespace esp32_ble_server_automations {

using namespace esp32_ble;

class BLETriggers {
 public:
#ifdef USE_ESP32_BLE_SERVER_CHARACTERISTIC_ON_WRITE
  static Trigger<std::vector<uint8_t>, uint16_t> *create_characteristic_on_write_trigger(
      BLECharacteristic *characteristic);
#endif
#ifdef USE_ESP32_BLE_SERVER_DESCRIPTOR_ON_WRITE
  static Trigger<std::vector<uint8_t>, uint16_t> *create_descriptor_on_write_trigger(BLEDescriptor *descriptor);
#endif
#ifdef USE_ESP32_BLE_SERVER_ON_CONNECT
  static Trigger<uint16_t> *create_server_on_connect_trigger(BLEServer *server);
#endif
#ifdef USE_ESP32_BLE_SERVER_ON_DISCONNECT
  static Trigger<uint16_t> *create_server_on_disconnect_trigger(BLEServer *server);
#endif
};

#ifdef USE_ESP32_BLE_SERVER_SET_VALUE_ACTION
// Ensures only one BLECharacteristicSetValueAction is active per characteristic.
class BLECharacteristicSetValueActionManager {
 public:
  static BLECharacteristicSetValueActionManager *get_instance() {
    static BLECharacteristicSetValueActionManager instance;
    return &instance;
  }
  void set_listener(BLECharacteristic *characteristic, const std::function<void()> &pre_notify_listener);
  bool has_listener(BLECharacteristic *characteristic) { return this->find_listener_(characteristic) != nullptr; }
  void emit_pre_notify(BLECharacteristic *characteristic) {
    for (const auto &entry : this->listeners_) {
      if (entry.characteristic == characteristic) {
        entry.pre_notify_listener();
        break;
      }
    }
  }

 private:
  struct ListenerEntry {
    BLECharacteristic *characteristic;
    std::function<void()> pre_notify_listener;
  };
  std::vector<ListenerEntry> listeners_;

  ListenerEntry *find_listener_(BLECharacteristic *characteristic);
  void remove_listener_(BLECharacteristic *characteristic);
};

template<typename... Ts> class BLECharacteristicSetValueAction : public Action<Ts...> {
 public:
  BLECharacteristicSetValueAction(BLECharacteristic *characteristic) : parent_(characteristic) {}
  TEMPLATABLE_VALUE(std::vector<uint8_t>, buffer)
  void set_buffer(std::initializer_list<uint8_t> buffer) { this->buffer_ = std::vector<uint8_t>(buffer); }
  void set_buffer(ByteBuffer buffer) { this->set_buffer(buffer.get_data()); }
  void play(const Ts &...x) override {
    if (BLECharacteristicSetValueActionManager::get_instance()->has_listener(this->parent_))
      return;
    this->parent_->set_value(this->buffer_.value(x...));
    this->parent_->on_read([this, x...](uint16_t id) { this->parent_->set_value(this->buffer_.value(x...)); });
    BLECharacteristicSetValueActionManager::get_instance()->set_listener(
        this->parent_, [this, x...]() { this->parent_->set_value(this->buffer_.value(x...)); });
  }

 protected:
  BLECharacteristic *parent_;
};
#endif  // USE_ESP32_BLE_SERVER_SET_VALUE_ACTION

#ifdef USE_ESP32_BLE_SERVER_NOTIFY_ACTION
template<typename... Ts> class BLECharacteristicNotifyAction : public Action<Ts...> {
 public:
  BLECharacteristicNotifyAction(BLECharacteristic *characteristic) : parent_(characteristic) {}
  void play(const Ts &...x) override {
#ifdef USE_ESP32_BLE_SERVER_SET_VALUE_ACTION
    BLECharacteristicSetValueActionManager::get_instance()->emit_pre_notify(this->parent_);
#endif
    this->parent_->notify();
  }

 protected:
  BLECharacteristic *parent_;
};
#endif  // USE_ESP32_BLE_SERVER_NOTIFY_ACTION

#ifdef USE_ESP32_BLE_SERVER_DESCRIPTOR_SET_VALUE_ACTION
template<typename... Ts> class BLEDescriptorSetValueAction : public Action<Ts...> {
 public:
  BLEDescriptorSetValueAction(BLEDescriptor *descriptor) : parent_(descriptor) {}
  TEMPLATABLE_VALUE(std::vector<uint8_t>, buffer)
  void set_buffer(std::initializer_list<uint8_t> buffer) { this->buffer_ = std::vector<uint8_t>(buffer); }
  void set_buffer(ByteBuffer buffer) { this->set_buffer(buffer.get_data()); }
  void play(const Ts &...x) override { this->parent_->set_value(this->buffer_.value(x...)); }

 protected:
  BLEDescriptor *parent_;
};
#endif  // USE_ESP32_BLE_SERVER_DESCRIPTOR_SET_VALUE_ACTION

}  // namespace esp32_ble_server_automations
}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
