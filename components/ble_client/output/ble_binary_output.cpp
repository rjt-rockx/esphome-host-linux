#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_binary_output.h"

#include "esphome/core/log.h"

namespace esphome {
namespace ble_client {

static const char *const TAG = "ble_binary_output";

void BLEBinaryOutput::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Binary Output:");
  ESP_LOGCONFIG(TAG, "  Service UUID: %s", this->service_uuid_.to_string().c_str());
  ESP_LOGCONFIG(TAG, "  Characteristic UUID: %s", this->char_uuid_.to_string().c_str());
}

void BLEBinaryOutput::on_disconnected(int /*reason*/) {
  this->node_state = espbt::ClientState::IDLE;
  this->char_handle_ = 0;
}

void BLEBinaryOutput::on_services_discovered() {
  auto *chr = this->parent()->get_characteristic(this->service_uuid_, this->char_uuid_);
  if (chr == nullptr) {
    ESP_LOGW(TAG, "characteristic %s not found", this->char_uuid_.to_string().c_str());
    return;
  }
  this->char_handle_ = chr->handle;
  this->char_props_ = chr->properties;
  // Prefer write-with-response if required AND supported; otherwise honor what
  // the characteristic advertises (WRITE → response, WRITE_NR → no response).
  if (this->require_response_ && (this->char_props_ & ESP_GATT_CHAR_PROP_BIT_WRITE)) {
    this->write_with_response_ = true;
  } else if (this->char_props_ & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) {
    this->write_with_response_ = false;
  } else {
    this->write_with_response_ = (this->char_props_ & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
  }
  this->node_state = espbt::ClientState::ESTABLISHED;
}

void BLEBinaryOutput::write_state(bool state) {
  if (this->char_handle_ == 0) {
    ESP_LOGW(TAG, "write_state: not connected/resolved");
    return;
  }
  uint8_t v = state ? 1 : 0;
  this->parent()->write_characteristic(this->char_handle_, &v, 1, this->write_with_response_);
}

void BLEBinaryOutput::on_characteristic_write(uint16_t handle, int status) {
  if (handle == this->char_handle_ && status != ESP_GATT_OK)
    ESP_LOGW(TAG, "write to 0x%04X failed: %s", handle, esp_err_to_name(status));
}

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
