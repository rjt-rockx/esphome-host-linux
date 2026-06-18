#ifdef USE_HOST

#include "ble.h"

#include "esphome/core/log.h"

namespace esphome {
namespace esp32_ble {

static const char *const TAG = "esp32_ble";

void ESP32BLE::advertising_start() {
  this->adv_.active = true;
  // Fire raw-advertisement callbacks (the beacon uses this to build its payload).
  for (auto &cb : this->raw_adv_callbacks_)
    cb(true);
  this->push_advertising_();
}

void ESP32BLE::advertising_set_service_data(const std::vector<uint8_t> &data) {
  this->adv_.service_data = data;
  this->push_advertising_();
}

void ESP32BLE::advertising_set_manufacturer_data(const std::vector<uint8_t> &data) {
  this->adv_.manufacturer_data = data;
  this->push_advertising_();
}

void ESP32BLE::advertising_set_service_data_and_name(std::span<const uint8_t> data, bool include_name) {
  this->adv_.service_data.assign(data.begin(), data.end());
  this->adv_.include_name = include_name;
  this->push_advertising_();
}

void ESP32BLE::advertising_add_service_uuid(ESPBTUUID uuid) {
  for (const auto &u : this->adv_.service_uuids)
    if (u == uuid)
      return;
  this->adv_.service_uuids.push_back(uuid);
  this->push_advertising_();
}

void ESP32BLE::advertising_remove_service_uuid(ESPBTUUID uuid) {
  auto &v = this->adv_.service_uuids;
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (*it == uuid) {
      v.erase(it);
      break;
    }
  }
  this->push_advertising_();
}

void ESP32BLE::advertising_register_raw_advertisement_callback(std::function<void(bool)> &&callback) {
  this->raw_adv_callbacks_.push_back(std::move(callback));
}

void ESP32BLE::push_advertising_() {
  if (this->adv_backend_ != nullptr)
    this->adv_backend_->on_advertising_changed(this->adv_);
}

}  // namespace esp32_ble
}  // namespace esphome

#endif  // USE_HOST
