#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_sensor.h"

#include "esphome/core/log.h"

#include <vector>

namespace esphome {
namespace ble_client {

static const char *const TAG = "ble_sensor";

void BLESensor::dump_config() {
  LOG_SENSOR("", "BLE Sensor", this);
  ESP_LOGCONFIG(TAG, "  Notify: %s", YESNO(this->notify_));
}

void BLESensor::on_disconnected(int /*reason*/) {
  this->status_set_warning();
  this->publish_state(NAN);
}

void BLESensor::on_services_discovered() {
  auto *chr = this->parent()->get_characteristic(this->service_uuid_, this->char_uuid_);
  if (chr == nullptr) {
    ESP_LOGW(TAG, "characteristic %s not found on service %s", this->char_uuid_.to_string().c_str(),
             this->service_uuid_.to_string().c_str());
    return;
  }
  // If a descriptor UUID is configured, target the descriptor handle for reads.
  if (this->descr_uuid_.length() > 0) {
    auto *descr = chr->get_descriptor(this->descr_uuid_);
    if (descr != nullptr) {
      this->handle = descr->handle;
      this->handle_is_descriptor_ = true;
    } else {
      this->handle = chr->handle;
    }
  } else {
    this->handle = chr->handle;
  }

  if (this->notify_) {
    // Subscribe; values arrive via on_notify (filtered by handle).
    chr->start_notify(nullptr);
  } else {
    this->node_state = espbt::ClientState::ESTABLISHED;
    this->update();  // initial poll
  }
}

void BLESensor::update() {
  if (this->node_state != espbt::ClientState::ESTABLISHED && this->parent()->state() != espbt::ClientState::ESTABLISHED)
    return;
  if (this->handle == 0)
    return;
  if (this->handle_is_descriptor_)
    this->parent()->read_descriptor(this->handle);
  else
    this->parent()->read_characteristic(this->handle);
}

void BLESensor::on_characteristic_read(const BLEReadResult &r) {
  if (this->handle_is_descriptor_ || r.handle != this->handle)
    return;
  this->status_clear_warning();
  this->publish_state(this->parse_data_(r.data, r.len));
}

void BLESensor::on_descriptor_read(const BLEReadResult &r) {
  if (!this->handle_is_descriptor_ || r.handle != this->handle)
    return;
  this->status_clear_warning();
  this->publish_state(this->parse_data_(r.data, r.len));
}

void BLESensor::on_notify(const BLENotifyEvent &e) {
  if (e.handle != this->handle)
    return;
  this->status_clear_warning();
  this->publish_state(this->parse_data_(e.data, e.len));
}

float BLESensor::parse_data_(const uint8_t *value, uint16_t value_len) {
  if (this->has_data_to_value_) {
    std::vector<uint8_t> data(value, value + value_len);
    return this->data_to_value_func_(data);
  }
  // Default: the first byte as a float (or NAN if empty).
  return value_len > 0 ? static_cast<float>(value[0]) : NAN;
}

// --- RSSI sensor ---
void BLEClientRSSISensor::dump_config() { LOG_SENSOR("", "BLE Client RSSI", this); }

void BLEClientRSSISensor::on_services_discovered() { this->node_state = espbt::ClientState::ESTABLISHED; }

void BLEClientRSSISensor::on_disconnected(int /*reason*/) {
  this->node_state = espbt::ClientState::IDLE;
  this->publish_state(NAN);
}

void BLEClientRSSISensor::update() {
  if (this->parent()->state() != espbt::ClientState::ESTABLISHED)
    return;
  this->parent()->read_rssi([this](int8_t rssi) { this->publish_state(rssi); });
}

void BLEClientRSSISensor::on_rssi(int8_t rssi) { this->publish_state(rssi); }

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
