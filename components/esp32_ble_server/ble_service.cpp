#ifdef USE_HOST

#include "ble_service.h"
#include "ble_server.h"

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/core/log.h"

namespace esphome {
namespace esp32_ble_server {

static const char *const TAG = "esp32_ble_server.service";

BLEService::BLEService(ESPBTUUID uuid, uint16_t num_handles, uint8_t inst_id, bool advertise)
    : uuid_(uuid), num_handles_(num_handles), inst_id_(inst_id), advertise_(advertise) {}

BLEService::~BLEService() {
  for (auto *chr : this->characteristics_)
    delete chr;  // NOLINT(cppcoreguidelines-owning-memory)
}

BLECharacteristic *BLEService::get_characteristic(ESPBTUUID uuid) {
  for (auto *chr : this->characteristics_) {
    if (chr->get_uuid() == uuid)
      return chr;
  }
  return nullptr;
}

BLECharacteristic *BLEService::get_characteristic(uint16_t uuid) {
  return this->get_characteristic(ESPBTUUID::from_uint16(uuid));
}
BLECharacteristic *BLEService::create_characteristic(uint16_t uuid, uint32_t properties) {
  return this->create_characteristic(ESPBTUUID::from_uint16(uuid), properties);
}
BLECharacteristic *BLEService::create_characteristic(const std::string &uuid, uint32_t properties) {
  return this->create_characteristic(ESPBTUUID::from_raw(uuid), properties);
}
BLECharacteristic *BLEService::create_characteristic(ESPBTUUID uuid, uint32_t properties) {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  BLECharacteristic *characteristic = new BLECharacteristic(uuid, properties);
  this->characteristics_.push_back(characteristic);
  this->last_created_characteristic_ = characteristic;
  return characteristic;
}

void BLEService::do_create(BLEServer *server) {
  // Cascade-create the characteristics (which cascade to descriptors) so the whole
  // tree is ready for one RegisterApplication.
  this->server_ = server;
  for (auto *characteristic : this->characteristics_) {
    characteristic->do_create(this);
  }
  this->state_ = CREATED;
}

void BLEService::start() {
  // The service is "running" once the application is registered. Add the advertised
  // UUID to the parent ESP32BLE so BlueZ includes it in the LEAdvertisement1
  // ServiceUUIDs (advertise: true in YAML).
  this->state_ = RUNNING;
  if (this->advertise_ && this->server_ != nullptr && this->server_->get_parent() != nullptr) {
    this->server_->get_parent()->advertising_add_service_uuid(this->uuid_);
  }
}

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
