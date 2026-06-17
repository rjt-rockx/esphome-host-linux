#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_text_sensor.h"

#include "esphome/core/log.h"

#include <cstdio>
#include <string>

namespace esphome {
namespace ble_client {

static const char *const TAG = "ble_text_sensor";

void BLETextSensor::dump_config() { LOG_TEXT_SENSOR("", "BLE Text Sensor", this); }

void BLETextSensor::on_disconnected(int /*reason*/) {
  this->node_state = espbt::ClientState::IDLE;
  this->publish_state("");
}

void BLETextSensor::on_services_discovered() {
  auto *chr = this->parent()->get_characteristic(this->service_uuid_, this->char_uuid_);
  if (chr == nullptr) {
    ESP_LOGW(TAG, "characteristic %s not found", this->char_uuid_.to_string().c_str());
    return;
  }
  this->handle = chr->handle;
  if (this->notify_) {
    chr->start_notify(nullptr);
  } else {
    this->node_state = espbt::ClientState::ESTABLISHED;
    this->update();
  }
}

void BLETextSensor::update() {
  if (this->handle == 0 || this->parent()->state() != espbt::ClientState::ESTABLISHED)
    return;
  this->parent()->read_characteristic(this->handle);
}

void BLETextSensor::on_characteristic_read(const BLEReadResult &r) {
  if (r.handle != this->handle)
    return;
  this->publish_state(this->parse_data_(r.data, r.len));
}

void BLETextSensor::on_notify(const BLENotifyEvent &e) {
  if (e.handle != this->handle)  // routing is by BLEClientBase instance — no conn_id needed
    return;
  this->publish_state(this->parse_data_(e.data, e.len));
}

std::string BLETextSensor::parse_data_(const uint8_t *value, uint16_t value_len) {
  // Default: hex string of the raw value (matches upstream's behavior).
  std::string out;
  char buf[4];
  for (uint16_t i = 0; i < value_len; i++) {
    std::snprintf(buf, sizeof(buf), "%02x", value[i]);
    out += buf;
  }
  return out;
}

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
