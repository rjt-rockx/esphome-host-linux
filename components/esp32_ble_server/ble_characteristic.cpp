#ifdef USE_HOST

#include "ble_characteristic.h"
#include "ble_server.h"
#include "ble_service.h"
#include "ble_gatt_server.h"

#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace esp32_ble_server {

static const char *const TAG = "esp32_ble_server.characteristic";

BLECharacteristic::~BLECharacteristic() {
  for (auto *descriptor : this->descriptors_) {
    delete descriptor;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

BLECharacteristic::BLECharacteristic(const ESPBTUUID uuid, uint32_t properties) : uuid_(uuid), properties_(0) {
  this->set_broadcast_property((properties & PROPERTY_BROADCAST) != 0);
  this->set_indicate_property((properties & PROPERTY_INDICATE) != 0);
  this->set_notify_property((properties & PROPERTY_NOTIFY) != 0);
  this->set_read_property((properties & PROPERTY_READ) != 0);
  this->set_write_property((properties & PROPERTY_WRITE) != 0);
  this->set_write_no_response_property((properties & PROPERTY_WRITE_NR) != 0);
}

void BLECharacteristic::set_value(ByteBuffer buffer) { this->set_value(buffer.get_data()); }
void BLECharacteristic::set_value(std::vector<uint8_t> &&buffer) { this->value_ = std::move(buffer); }
void BLECharacteristic::set_value(std::initializer_list<uint8_t> data) { this->set_value(std::vector<uint8_t>(data)); }
void BLECharacteristic::set_value(const std::string &buffer) {
  this->set_value(std::vector<uint8_t>(buffer.begin(), buffer.end()));
}

void BLECharacteristic::notify() {
  if (this->gatt_server_ == nullptr)
    return;
  // Emitting a Value PropertiesChanged on the worker thread; BlueZ delivers to
  // subscribed (StartNotify'd) clients only. The server marshals main->worker.
  this->gatt_server_->notify_characteristic(this, this->value_);
}

void BLECharacteristic::add_descriptor(BLEDescriptor *descriptor) {
  // No CCCD write tracking on host: BlueZ owns the 0x2902 and StartNotify/
  // StopNotify drive subscription. The server filters the 0x2902 at export.
  this->descriptors_.push_back(descriptor);
}

void BLECharacteristic::remove_descriptor(BLEDescriptor *descriptor) {
  this->descriptors_.erase(std::remove(this->descriptors_.begin(), this->descriptors_.end(), descriptor),
                           this->descriptors_.end());
}

void BLECharacteristic::do_create(BLEService *service) {
  this->service_ = service;
  for (auto *descriptor : this->descriptors_) {
    descriptor->do_create(this);
  }
  this->state_ = CREATED;
}

bool BLECharacteristic::is_created() { return this->state_ == CREATED; }
bool BLECharacteristic::is_failed() { return this->state_ == FAILED; }

void BLECharacteristic::set_property_bit_(uint32_t bit, bool value) {
  if (value) {
    this->properties_ |= bit;
  } else {
    this->properties_ &= ~bit;
  }
}

void BLECharacteristic::set_broadcast_property(bool value) { this->set_property_bit_(PROPERTY_BROADCAST, value); }
void BLECharacteristic::set_indicate_property(bool value) { this->set_property_bit_(PROPERTY_INDICATE, value); }
void BLECharacteristic::set_notify_property(bool value) { this->set_property_bit_(PROPERTY_NOTIFY, value); }
void BLECharacteristic::set_read_property(bool value) { this->set_property_bit_(PROPERTY_READ, value); }
void BLECharacteristic::set_write_property(bool value) { this->set_property_bit_(PROPERTY_WRITE, value); }
void BLECharacteristic::set_write_no_response_property(bool value) {
  this->set_property_bit_(PROPERTY_WRITE_NR, value);
}

const std::vector<uint8_t> &BLECharacteristic::host_on_read(uint16_t conn_id) {
  if (this->on_read_callback_) {
    (*this->on_read_callback_)(conn_id);
  }
  return this->value_;
}

void BLECharacteristic::host_on_write(std::span<const uint8_t> data, uint16_t conn_id) {
  this->value_.assign(data.begin(), data.end());
  if (this->on_write_callback_) {
    (*this->on_write_callback_)(data, conn_id);
  }
}

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
