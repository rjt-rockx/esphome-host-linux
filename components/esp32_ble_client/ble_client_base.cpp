#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_client_base.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdio>

#ifdef USE_HOST
#include "ble_gatt_host.h"
#endif

namespace esphome {
namespace esp32_ble_client {

static const char *const TAG = "ble_client_base";

BLEClientBase::BLEClientBase() = default;
BLEClientBase::~BLEClientBase() = default;  // BLEGattHost complete here

float BLEClientBase::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

void BLEClientBase::setup() {
  this->set_state(espbt::ClientState::IDLE);
  this->conn_id_ = UNSET_CONN_ID;
}

void BLEClientBase::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Client (host):");
  ESP_LOGCONFIG(TAG, "  Address: %s", this->address_str_);
  ESP_LOGCONFIG(TAG, "  Adapter: %s", this->adapter_.c_str());
}

void BLEClientBase::run_later(std::function<void()> &&f) {  // NOLINT
  this->defer(std::move(f));
}

bool BLEClientBase::parse_device(const espbt::ESPBTDevice &device) {
  if (this->address_ == 0)
    return false;
  if (device.address_uint64() != this->address_)
    return false;
  // Found our device while scanning.
  if (this->state() == espbt::ClientState::IDLE && this->auto_connect_) {
    this->set_state(espbt::ClientState::DISCOVERED);
  }
  return true;
}

void BLEClientBase::set_address(uint64_t address) {
  this->address_ = address;
  for (int i = 0; i < 6; i++)
    this->remote_bda_[i] = (address >> ((5 - i) * 8)) & 0xFF;
  if (address == 0) {
    this->address_str_[0] = '\0';
    this->device_path_.clear();
    return;
  }
  std::snprintf(this->address_str_, sizeof(this->address_str_), "%02X:%02X:%02X:%02X:%02X:%02X", this->remote_bda_[0],
                this->remote_bda_[1], this->remote_bda_[2], this->remote_bda_[3], this->remote_bda_[4],
                this->remote_bda_[5]);
  // BlueZ device object path: /org/bluez/<adapter>/dev_AA_BB_CC_DD_EE_FF
  char dev[32];
  std::snprintf(dev, sizeof(dev), "dev_%02X_%02X_%02X_%02X_%02X_%02X", this->remote_bda_[0], this->remote_bda_[1],
                this->remote_bda_[2], this->remote_bda_[3], this->remote_bda_[4], this->remote_bda_[5]);
  this->device_path_ = "/org/bluez/" + this->adapter_ + "/" + dev;
}

void BLEClientBase::connect() {
  ESP_LOGI(TAG, "[%s] Connecting", this->address_str_);
  this->set_state(espbt::ClientState::CONNECTING);
  this->connecting_started_ = millis();
#ifdef USE_HOST
  if (!this->host_)
    this->host_ = std::make_unique<BLEGattHost>(this->adapter_, this->device_path_);
  this->host_->connect();
#endif
}

void BLEClientBase::disconnect() {
  if (this->state() == espbt::ClientState::IDLE)
    return;
  if (this->state() == espbt::ClientState::CONNECTING) {
    // Defer until we know the connection is up, then tear down.
    this->want_disconnect_ = true;
    return;
  }
  this->unconditional_disconnect();
}

void BLEClientBase::unconditional_disconnect() {
  this->set_disconnecting_();
#ifdef USE_HOST
  if (this->host_)
    this->host_->disconnect();
#endif
}

void BLEClientBase::set_disconnecting_() {
  this->disconnecting_started_ = millis();
  this->set_state(espbt::ClientState::DISCONNECTING);
}

void BLEClientBase::release_services() {
  for (auto *svc : this->services_)
    delete svc;  // NOLINT(cppcoreguidelines-owning-memory)
  this->services_.clear();
}

void BLEClientBase::set_state(espbt::ClientState st) { espbt::ESPBTClient::set_state(st); }

void BLEClientBase::loop() {
#ifdef USE_HOST
  if (this->host_) {
    auto drained = this->host_->drain_events();
    for (auto &ev : drained)
      this->dispatch_event_(ev);
  }
#endif
  // CONNECTING timeout → treat as a disconnect (state returns to IDLE).
  if (this->state() == espbt::ClientState::CONNECTING && (millis() - this->connecting_started_) > CONNECT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "[%s] Connect timeout", this->address_str_);
#ifdef USE_HOST
    if (this->host_)
      this->host_->disconnect();
#endif
    HostGattEvent ev;
    ev.kind = HostGattEvent::Kind::DISCONNECTED;
    ev.disc_reason = ESP_GATT_CONN_TIMEOUT;
    this->dispatch_event_(ev);
  }
  // DISCONNECTING safety timeout.
  if (this->state() == espbt::ClientState::DISCONNECTING &&
      (millis() - this->disconnecting_started_) > DISCONNECT_TIMEOUT_MS) {
    HostGattEvent ev;
    ev.kind = HostGattEvent::Kind::DISCONNECTED;
    ev.disc_reason = ESP_GATT_CONN_TIMEOUT;
    this->dispatch_event_(ev);
  }
}

void BLEClientBase::dispatch_event_(const HostGattEvent &ev) {
  switch (ev.kind) {
    case HostGattEvent::Kind::CONNECTED:
      if (this->want_disconnect_) {
        // disconnect() was requested mid-connect; tear down now.
        this->unconditional_disconnect();
        break;
      }
      ESP_LOGI(TAG, "[%s] Connected", this->address_str_);
      this->set_state(espbt::ClientState::CONNECTED);
      // Step 1: no discovery yet — service discovery + ESTABLISHED arrive in Step 2.
      break;
    case HostGattEvent::Kind::SERVICES_DISCOVERED: {
      this->mtu_ = ev.mtu;
      // Build the wrapper tree (main-thread-owned) from the worker's snapshot.
      this->release_services();
      for (const auto &ds : ev.services) {
        auto *svc = new BLEService();  // NOLINT(cppcoreguidelines-owning-memory)
        svc->uuid = ds.uuid;
        svc->start_handle = ds.start_handle;
        svc->end_handle = ds.end_handle;
        svc->client = this;
        svc->parsed = true;
        for (const auto &dc : ds.characteristics) {
          auto *chr = new BLECharacteristic();  // NOLINT(cppcoreguidelines-owning-memory)
          chr->uuid = dc.uuid;
          chr->handle = dc.handle;
          chr->properties = dc.properties;
          chr->service = svc;
          chr->parsed = true;
          for (const auto &dd : dc.descriptors) {
            auto *desc = new BLEDescriptor();  // NOLINT(cppcoreguidelines-owning-memory)
            desc->uuid = dd.uuid;
            desc->handle = dd.handle;
            desc->characteristic = chr;
            chr->descriptors.push_back(desc);
          }
          svc->characteristics.push_back(chr);
        }
        this->services_.push_back(svc);
      }
      ESP_LOGI(TAG, "[%s] Services resolved: %zu services, MTU %u", this->address_str_, this->services_.size(),
               this->mtu_);
      for (auto *svc : this->services_) {
        char sbuf[espbt::ESPBTUUID::UUID_STR_LEN];
        ESP_LOGD(TAG, "  SVC %s", svc->uuid.to_str(sbuf));
        for (auto *chr : svc->characteristics) {
          char cbuf[espbt::ESPBTUUID::UUID_STR_LEN];
          ESP_LOGD(TAG, "    CHAR %s handle=0x%04X props=0x%02X", chr->uuid.to_str(cbuf), chr->handle, chr->properties);
          for (auto *d : chr->descriptors) {
            char dbuf[espbt::ESPBTUUID::UUID_STR_LEN];
            ESP_LOGD(TAG, "      DESC %s handle=0x%04X", d->uuid.to_str(dbuf), d->handle);
          }
        }
      }
      this->set_state(espbt::ClientState::ESTABLISHED);
      break;
    }
    case HostGattEvent::Kind::DISCONNECTED: {
      ESP_LOGI(TAG, "[%s] Disconnected (reason %d)", this->address_str_, ev.disc_reason);
      this->release_services();
      this->set_idle_();
      this->on_disconnect_complete(ev.disc_reason);
      break;
    }
    case HostGattEvent::Kind::RSSI:
      if (this->rssi_cb_)
        this->rssi_cb_(ev.rssi);
      break;
    case HostGattEvent::Kind::PAIRING_COMPLETE:
      this->paired_ = ev.pairing_success;
      break;
    default:
      break;
  }
}

// --- lookups (operate on the eagerly-built services_ tree) ---
BLEService *BLEClientBase::get_service(espbt::ESPBTUUID uuid) {
  for (auto *svc : this->services_)
    if (svc->uuid == uuid)
      return svc;
  return nullptr;
}
BLEService *BLEClientBase::get_service(uint16_t uuid) { return this->get_service(espbt::ESPBTUUID::from_uint16(uuid)); }

BLECharacteristic *BLEClientBase::get_characteristic(espbt::ESPBTUUID service, espbt::ESPBTUUID chr) {
  auto *svc = this->get_service(service);
  if (svc == nullptr)
    return nullptr;
  for (auto *c : svc->characteristics)
    if (c->uuid == chr)
      return c;
  return nullptr;
}
BLECharacteristic *BLEClientBase::get_characteristic(uint16_t service, uint16_t chr) {
  return this->get_characteristic(espbt::ESPBTUUID::from_uint16(service), espbt::ESPBTUUID::from_uint16(chr));
}
BLECharacteristic *BLEClientBase::get_characteristic(uint16_t handle) {
  for (auto *svc : this->services_)
    for (auto *c : svc->characteristics)
      if (c->handle == handle)
        return c;
  return nullptr;
}
BLEDescriptor *BLEClientBase::get_descriptor(espbt::ESPBTUUID service, espbt::ESPBTUUID chr, espbt::ESPBTUUID descr) {
  auto *c = this->get_characteristic(service, chr);
  if (c == nullptr)
    return nullptr;
  for (auto *d : c->descriptors)
    if (d->uuid == descr)
      return d;
  return nullptr;
}
BLEDescriptor *BLEClientBase::get_descriptor(uint16_t handle) {
  for (auto *svc : this->services_)
    for (auto *c : svc->characteristics)
      for (auto *d : c->descriptors)
        if (d->handle == handle)
          return d;
  return nullptr;
}
BLEDescriptor *BLEClientBase::get_config_descriptor(uint16_t handle) {
  // CCCD (0x2902) for the characteristic at `handle`.
  auto *c = this->get_characteristic(handle);
  if (c == nullptr)
    return nullptr;
  for (auto *d : c->descriptors)
    if (d->uuid == espbt::ESPBTUUID::from_uint16(0x2902))
      return d;
  return nullptr;
}

float BLEClientBase::parse_char_value(uint8_t *value, uint16_t length) {
  if (length == 0)
    return 0.0f;
  // Mirror upstream's GATT presentation-format-ish parse for simple types.
  if (length == 1)
    return static_cast<float>(value[0]);
  if (length == 2)
    return static_cast<float>(static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8));
  if (length == 4) {
    uint32_t v = static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8) |
                 (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
    return static_cast<float>(v);
  }
  return static_cast<float>(value[0]);
}

bool BLEClientBase::check_addr_(const uint8_t bda[6]) { return std::memcmp(bda, this->remote_bda_, 6) == 0; }

esp_err_t BLEClientBase::pair() {
#ifdef USE_HOST
  if (this->host_)
    this->host_->pair();
#endif
  return ESP_GATT_OK;
}

// --- handle primitives: route to the BlueZ worker ---
esp_err_t BLEClientBase::read_characteristic(uint16_t handle) {
#ifdef USE_HOST
  if (this->state() != espbt::ClientState::ESTABLISHED || !this->host_)
    return ESP_GATT_NOT_CONNECTED;
  this->host_->read_char(handle);
  return ESP_GATT_OK;
#else
  return ESP_GATT_NOT_CONNECTED;
#endif
}
esp_err_t BLEClientBase::write_characteristic(uint16_t handle, const uint8_t *data, size_t length, bool response) {
#ifdef USE_HOST
  if (this->state() != espbt::ClientState::ESTABLISHED || !this->host_)
    return ESP_GATT_NOT_CONNECTED;
  this->host_->write_char(handle, data, length, response);
  return ESP_GATT_OK;
#else
  return ESP_GATT_NOT_CONNECTED;
#endif
}
esp_err_t BLEClientBase::read_descriptor(uint16_t handle) {
#ifdef USE_HOST
  if (this->state() != espbt::ClientState::ESTABLISHED || !this->host_)
    return ESP_GATT_NOT_CONNECTED;
  this->host_->read_desc(handle);
  return ESP_GATT_OK;
#else
  return ESP_GATT_NOT_CONNECTED;
#endif
}
esp_err_t BLEClientBase::write_descriptor(uint16_t handle, const uint8_t *data, size_t length, bool response) {
#ifdef USE_HOST
  if (this->state() != espbt::ClientState::ESTABLISHED || !this->host_)
    return ESP_GATT_NOT_CONNECTED;
  this->host_->write_desc(handle, data, length, response);
  return ESP_GATT_OK;
#else
  return ESP_GATT_NOT_CONNECTED;
#endif
}
esp_err_t BLEClientBase::notify_characteristic(uint16_t handle, bool enable) {
#ifdef USE_HOST
  if (this->state() != espbt::ClientState::ESTABLISHED || !this->host_)
    return ESP_GATT_NOT_CONNECTED;
  this->host_->set_notify(handle, enable);
  return ESP_GATT_OK;
#else
  return ESP_GATT_NOT_CONNECTED;
#endif
}
esp_err_t BLEClientBase::passkey_reply(uint32_t passkey) {
#ifdef USE_HOST
  if (this->host_)
    this->host_->passkey_reply(passkey);
#endif
  return ESP_GATT_OK;
}
esp_err_t BLEClientBase::confirm_reply(bool accept) {
#ifdef USE_HOST
  if (this->host_)
    this->host_->confirm_reply(accept);
#endif
  return ESP_GATT_OK;
}
esp_err_t BLEClientBase::remove_bond() {
#ifdef USE_HOST
  if (this->host_)
    this->host_->remove_bond();
#endif
  return ESP_GATT_OK;
}
void BLEClientBase::read_rssi(std::function<void(int8_t)> &&cb) {
  this->rssi_cb_ = std::move(cb);
#ifdef USE_HOST
  if (this->host_ && this->state() == espbt::ClientState::ESTABLISHED)
    this->host_->read_rssi();
#endif
}

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
