#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_characteristic.h"
#include "ble_service.h"
#include "ble_client_base.h"

namespace esphome {
namespace esp32_ble_client {

BLECharacteristic::~BLECharacteristic() { this->release_descriptors(); }

void BLECharacteristic::release_descriptors() {
  for (auto *d : this->descriptors)
    delete d;  // NOLINT(cppcoreguidelines-owning-memory)
  this->descriptors.clear();
}

BLEDescriptor *BLECharacteristic::get_descriptor(espbt::ESPBTUUID uuid) {
  for (auto *d : this->descriptors)
    if (d->uuid == uuid)
      return d;
  return nullptr;
}
BLEDescriptor *BLECharacteristic::get_descriptor(uint16_t uuid) {
  return this->get_descriptor(espbt::ESPBTUUID::from_uint16(uuid));
}
BLEDescriptor *BLECharacteristic::get_descriptor_by_handle(uint16_t handle) {
  for (auto *d : this->descriptors)
    if (d->handle == handle)
      return d;
  return nullptr;
}

esp_err_t BLECharacteristic::write_value(uint8_t *new_val, int16_t new_val_size) {
  return this->write_value(new_val, new_val_size, ESP_GATT_WRITE_TYPE_NO_RSP);
}

esp_err_t BLECharacteristic::write_value(uint8_t *new_val, int16_t new_val_size, esp_gatt_write_type_t write_type) {
  if (this->service == nullptr || this->service->client == nullptr)
    return ESP_GATT_NOT_CONNECTED;
  bool response = (write_type == ESP_GATT_WRITE_TYPE_RSP);
  return this->service->client->write_characteristic(this->handle, new_val, static_cast<size_t>(new_val_size),
                                                     response);
}

esp_err_t BLECharacteristic::read_value(std::function<void(const BLEReadResult &)> && /*cb*/) {
  // Routing of the per-char callback lands in Step 3; for now trigger the read.
  if (this->service == nullptr || this->service->client == nullptr)
    return ESP_GATT_NOT_CONNECTED;
  return this->service->client->read_characteristic(this->handle);
}

esp_err_t BLECharacteristic::start_notify(std::function<void(const BLENotifyEvent &)> && /*cb*/) {
  if (this->service == nullptr || this->service->client == nullptr)
    return ESP_GATT_NOT_CONNECTED;
  return this->service->client->notify_characteristic(this->handle, true);
}

esp_err_t BLECharacteristic::stop_notify() {
  if (this->service == nullptr || this->service->client == nullptr)
    return ESP_GATT_NOT_CONNECTED;
  return this->service->client->notify_characteristic(this->handle, false);
}

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
