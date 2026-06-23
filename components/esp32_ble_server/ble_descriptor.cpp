#ifdef USE_HOST

#include "ble_descriptor.h"
#include "ble_characteristic.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace esp32_ble_server {

static const char *const TAG = "esp32_ble_server.descriptor";

BLEDescriptor::BLEDescriptor(ESPBTUUID uuid, uint16_t max_len, bool read, bool write)
    : uuid_(uuid), max_len_(max_len), read_(read), write_(write) {}

BLEDescriptor::~BLEDescriptor() = default;

void BLEDescriptor::do_create(BLECharacteristic *characteristic) {
  this->characteristic_ = characteristic;
  this->state_ = CREATED;
}

void BLEDescriptor::set_value(std::vector<uint8_t> &&buffer) {
  this->set_value_impl_(buffer.data(), buffer.size());
}

void BLEDescriptor::set_value(std::initializer_list<uint8_t> data) {
  this->set_value_impl_(data.begin(), data.size());
}

void BLEDescriptor::set_value_impl_(const uint8_t *data, size_t length) {
  if (length > this->max_len_) {
    ESP_LOGE(TAG, "Size %zu too large, must be no bigger than %u", length, this->max_len_);
    return;
  }
  this->value_.assign(data, data + length);
}

void BLEDescriptor::host_on_write(std::span<const uint8_t> data, uint16_t conn_id) {
  this->value_.assign(data.begin(), data.end());
  if (this->on_write_callback_) {
    (*this->on_write_callback_)(data, conn_id);
  }
}

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
