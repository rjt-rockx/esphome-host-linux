#ifdef USE_HOST

#include "ble_server.h"
#include "ble_gatt_server.h"

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace esp32_ble_server {

static const char *const TAG = "esp32_ble_server";

void BLEServer::setup() {
  global_ble_server = this;

  // Build the whole GATT object tree and register it with BlueZ in one shot
  // (RegisterApplication collapses the IDF per-characteristic create/start dance).
  // DIS goes first so it is at the top of the table (cosmetic on BlueZ).
  if (this->device_information_service_ != nullptr) {
    this->device_information_service_->do_create(this);
  }
  for (auto &entry : this->services_) {
    if (entry.service == this->device_information_service_)
      continue;
    entry.service->do_create(this);
  }
  // Mark every queued service "running" (advertise UUIDs get pushed to the parent).
  for (auto *service : this->services_to_start_) {
    service->start();
  }
  if (this->device_information_service_ != nullptr)
    this->device_information_service_->start();

  std::vector<BLEService *> all;
  all.reserve(this->services_.size());
  if (this->device_information_service_ != nullptr)
    all.push_back(this->device_information_service_);
  for (auto &entry : this->services_) {
    if (entry.service != this->device_information_service_)
      all.push_back(entry.service);
  }

  this->gatt_server_ = make_unique<BLEGattServer>(this);
  // Register as the parent ESP32BLE's advertising backend BEFORE start() so any
  // advertising_* the services triggered (advertise: true) is captured, and so
  // advertising_set_appearance/manufacturer_data from codegen reach BlueZ.
  this->parent_->set_advertising_backend(this->gatt_server_.get());
  this->gatt_server_->start(all);
  this->state_ = RUNNING;
  // Kick advertising once the app is registered so the LEAdvertisement1 is built
  // from the accumulated advertisement (appearance, service UUIDs, mfg data).
  this->parent_->advertising_start();
  ESP_LOGD(TAG, "BLE server set up (%u services)", static_cast<unsigned>(all.size()));
}

void BLEServer::loop() {
  if (this->gatt_server_ == nullptr)
    return;
  auto events = this->gatt_server_->drain_events();
  for (auto &ev : events) {
    switch (ev.kind) {
      case ServerEvent::Kind::CONNECT:
        this->host_on_connect(ev.conn_id);
        break;
      case ServerEvent::Kind::DISCONNECT:
        this->host_on_disconnect(ev.conn_id);
        break;
    }
  }
}

bool BLEServer::can_proceed() { return this->state_ == RUNNING; }

float BLEServer::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH + 10; }

void BLEServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32 BLE Server (host/BlueZ):\n"
                "  Max clients: %u",
                this->max_clients_);
}

void BLEServer::restart_advertising_() {
  if (this->is_running() && this->parent_ != nullptr) {
    this->parent_->advertising_set_manufacturer_data(this->manufacturer_data_);
  }
}

BLEService *BLEServer::create_service(ESPBTUUID uuid, bool advertise, uint16_t num_handles) {
  // Pick the first free inst_id for this UUID (matches upstream).
  uint8_t inst_id = 0;
  for (; inst_id < 0xFF; inst_id++) {
    if (this->get_service(uuid, inst_id) == nullptr)
      break;
  }
  if (inst_id == 0xFF) {
    ESP_LOGW(TAG, "Could not create BLE service, too many instances");
    return nullptr;
  }
  BLEService *service =  // NOLINT(cppcoreguidelines-owning-memory)
      new BLEService(uuid, num_handles, inst_id, advertise);
  this->services_.push_back({uuid, inst_id, service});
  return service;
}

void BLEServer::remove_service(ESPBTUUID uuid, uint8_t inst_id) {
  for (auto it = this->services_.begin(); it != this->services_.end(); ++it) {
    if (it->uuid == uuid && it->inst_id == inst_id) {
      it->service->do_delete();
      delete it->service;  // NOLINT(cppcoreguidelines-owning-memory)
      this->services_.erase(it);
      return;
    }
  }
}

BLEService *BLEServer::get_service(ESPBTUUID uuid, uint8_t inst_id) {
  for (auto &entry : this->services_) {
    if (entry.uuid == uuid && entry.inst_id == inst_id)
      return entry.service;
  }
  return nullptr;
}

void BLEServer::dispatch_callbacks_(CallbackType type, uint16_t conn_id) {
  for (auto &entry : this->callbacks_) {
    if (entry.type == type)
      entry.callback(conn_id);
  }
}

void BLEServer::host_on_connect(uint16_t conn_id) {
  if (std::find(this->clients_.begin(), this->clients_.end(), conn_id) != this->clients_.end())
    return;
  this->clients_.push_back(conn_id);
  ESP_LOGD(TAG, "BLE client connected (conn_id=%u, %u total)", conn_id, static_cast<unsigned>(this->clients_.size()));
  this->dispatch_callbacks_(CallbackType::ON_CONNECT, conn_id);
}

void BLEServer::host_on_disconnect(uint16_t conn_id) {
  auto it = std::find(this->clients_.begin(), this->clients_.end(), conn_id);
  if (it == this->clients_.end())
    return;
  this->clients_.erase(it);
  ESP_LOGD(TAG, "BLE client disconnected (conn_id=%u, %u left)", conn_id, static_cast<unsigned>(this->clients_.size()));
  this->dispatch_callbacks_(CallbackType::ON_DISCONNECT, conn_id);
}

BLEServer *global_ble_server = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
