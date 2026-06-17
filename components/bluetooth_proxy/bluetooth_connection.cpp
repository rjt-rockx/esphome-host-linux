#if defined(USE_ESP32) || defined(USE_HOST)

#include "bluetooth_connection.h"

#include "esphome/components/api/api_pb2.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "bluetooth_proxy.h"

namespace esphome {
namespace bluetooth_proxy {

using esp32_ble::ESP_UUID_LEN_16;
using esp32_ble::ESP_UUID_LEN_32;
using esp32_ble::ESP_UUID_LEN_128;

static const char *const TAG = "bluetooth_proxy.connection";

// --- UUID packing helpers (pure; identical to upstream — no IDF GATT calls) ---
static void fill_128bit_uuid_array(std::array<uint64_t, 2> &out, esp_bt_uuid_t uuid_source) {
  out[0] = uuid_source.len == ESP_UUID_LEN_128
               ? (((uint64_t) uuid_source.uuid.uuid128[15] << 56) | ((uint64_t) uuid_source.uuid.uuid128[14] << 48) |
                  ((uint64_t) uuid_source.uuid.uuid128[13] << 40) | ((uint64_t) uuid_source.uuid.uuid128[12] << 32) |
                  ((uint64_t) uuid_source.uuid.uuid128[11] << 24) | ((uint64_t) uuid_source.uuid.uuid128[10] << 16) |
                  ((uint64_t) uuid_source.uuid.uuid128[9] << 8) | ((uint64_t) uuid_source.uuid.uuid128[8]))
               : (((uint64_t) (uuid_source.len == ESP_UUID_LEN_16 ? uuid_source.uuid.uuid16 : uuid_source.uuid.uuid32)
                   << 32) |
                  0x00001000ULL);
  out[1] = uuid_source.len == ESP_UUID_LEN_128
               ? ((uint64_t) uuid_source.uuid.uuid128[7] << 56) | ((uint64_t) uuid_source.uuid.uuid128[6] << 48) |
                     ((uint64_t) uuid_source.uuid.uuid128[5] << 40) | ((uint64_t) uuid_source.uuid.uuid128[4] << 32) |
                     ((uint64_t) uuid_source.uuid.uuid128[3] << 24) | ((uint64_t) uuid_source.uuid.uuid128[2] << 16) |
                     ((uint64_t) uuid_source.uuid.uuid128[1] << 8) | ((uint64_t) uuid_source.uuid.uuid128[0])
               : 0x800000805F9B34FBULL;
}

static void fill_gatt_uuid(std::array<uint64_t, 2> &uuid_128, uint32_t &short_uuid, const esp_bt_uuid_t &uuid,
                           bool use_efficient_uuids) {
  if (!use_efficient_uuids || uuid.len == ESP_UUID_LEN_128) {
    fill_128bit_uuid_array(uuid_128, uuid);
  } else if (uuid.len == ESP_UUID_LEN_16) {
    short_uuid = uuid.uuid.uuid16;
  } else if (uuid.len == ESP_UUID_LEN_32) {
    short_uuid = uuid.uuid.uuid32;
  }
}

bool BluetoothConnection::supports_efficient_uuids_() const {
  auto *api_conn = this->proxy_->get_api_connection();
  return api_conn && api_conn->client_supports_api_version(1, 12);
}

void BluetoothConnection::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Connection (host):");
  BLEClientBase::dump_config();
}

// --- slot accounting (pure; identical to upstream) ---
void BluetoothConnection::update_allocated_slot_(uint64_t find_value, uint64_t set_value) {
  auto &allocated = this->proxy_->connections_free_response_.allocated;
  for (auto &slot : allocated) {
    if (slot == find_value) {
      slot = set_value;
      return;
    }
  }
}

void BluetoothConnection::set_address(uint64_t address) {
  if (address == 0 && this->address_ != 0) {
    this->proxy_->connections_free_response_.free++;
    this->update_allocated_slot_(this->address_, 0);
  } else if (address != 0 && this->address_ == 0) {
    this->proxy_->connections_free_response_.free--;
    this->update_allocated_slot_(0, address);
  }
  BLEClientBase::set_address(address);
}

void BluetoothConnection::loop() {
  BLEClientBase::loop();
  if (this->address_ == 0)
    return;
  if (this->send_service_ >= 0 && this->send_service_ <= (int16_t) this->services_.size())
    this->send_service_for_discovery_();
}

// Consume worker events directly (the proxy connection is not a node). The base
// dispatch_event_ runs the ClientState machine + builds the service tree; we
// then fan each result to aioesphomeapi.
void BluetoothConnection::dispatch_event_(const esp32_ble_client::HostGattEvent &ev) {
  using esp32_ble_client::HostGattEvent;
  BLEClientBase::dispatch_event_(ev);  // advance state machine / build services_

  auto *api_connection = this->proxy_->get_api_connection();
  switch (ev.kind) {
    case HostGattEvent::Kind::SERVICES_DISCOVERED:
      // MTU + services are ready together on host. Tell HA we're connected with
      // the real MTU. Do NOT auto-start streaming the service DB here — that
      // matches upstream: send_service_ stays at INIT_SENDING_SERVICES until HA
      // explicitly asks via bluetooth_gatt_send_services (which sets it to 0).
      // Auto-starting here streamed services before HA requested them, so by the
      // time HA called get_services send_service_ was already DONE → 0 services.
      this->proxy_->send_device_connection(this->address_, true, this->mtu_);
      this->proxy_->send_connections_free();
      break;
    case HostGattEvent::Kind::READ_COMPLETE:
    case HostGattEvent::Kind::DESC_READ:
      if (ev.status != ESP_GATT_OK) {
        this->proxy_->send_gatt_error(this->address_, ev.handle, ev.status);
      } else if (api_connection != nullptr) {
        api::BluetoothGATTReadResponse resp;
        resp.address = this->address_;
        resp.handle = ev.handle;
        resp.set_data(ev.data.get(), ev.len);
        api_connection->send_message(resp);
      }
      break;
    case HostGattEvent::Kind::WRITE_COMPLETE:
    case HostGattEvent::Kind::DESC_WRITE:
      if (ev.status != ESP_GATT_OK) {
        this->proxy_->send_gatt_error(this->address_, ev.handle, ev.status);
      } else if (api_connection != nullptr) {
        api::BluetoothGATTWriteResponse resp;
        resp.address = this->address_;
        resp.handle = ev.handle;
        api_connection->send_message(resp);
      }
      break;
    case HostGattEvent::Kind::NOTIFY_REGISTERED:
      if (ev.status != ESP_GATT_OK) {
        this->proxy_->send_gatt_error(this->address_, ev.handle, ev.status);
      } else if (api_connection != nullptr) {
        api::BluetoothGATTNotifyResponse resp;
        resp.address = this->address_;
        resp.handle = ev.handle;
        api_connection->send_message(resp);
      }
      break;
    case HostGattEvent::Kind::NOTIFY:
      if (api_connection != nullptr) {
        api::BluetoothGATTNotifyDataResponse resp;
        resp.address = this->address_;
        resp.handle = ev.handle;
        resp.set_data(ev.data.get(), ev.len);
        api_connection->send_message(resp);
      }
      break;
    case HostGattEvent::Kind::PAIRING_COMPLETE:
      this->proxy_->send_device_pairing(this->address_, ev.pairing_success,
                                        ev.pairing_success ? ESP_OK : ev.disc_reason);
      break;
    default:
      break;  // CONNECTED/DISCONNECTED handled by base + on_disconnect_complete
  }
}

void BluetoothConnection::on_disconnect_complete(esp_err_t reason) {
  if (this->address_ == 0)
    return;
  ESP_LOGD(TAG, "[%d] [%s] Close, reason=0x%02x, freeing slot", this->connection_index_, this->address_str(), reason);
  this->reset_connection_(reason);
}

void BluetoothConnection::reset_connection_(esp_err_t reason) {
  this->proxy_->send_device_connection(this->address_, false, 0, reason);
  // Do NOT send services_done on an interrupted discovery — aioesphomeapi times
  // out and retries (matches upstream).
  this->set_address(0);
  this->send_service_ = -3;  // INIT_SENDING_SERVICES
  this->seen_services_ = false;
  this->proxy_->send_connections_free();
}

// Build BluetoothGATTGetServicesResponse from the cached service tree (the host
// worker already walked it — no IDF DB offset-walk). Batched to MAX_PACKET_SIZE,
// one or more services per message, mirroring upstream's wire format.
void BluetoothConnection::send_service_for_discovery_() {
  if (this->send_service_ < 0 || (size_t) this->send_service_ >= this->services_.size()) {
    this->send_service_ = -2;  // DONE_SENDING_SERVICES
    this->proxy_->send_gatt_services_done(this->address_);
    this->release_services();
    return;
  }
  auto *api_conn = this->proxy_->get_api_connection();
  if (api_conn == nullptr) {
    this->send_service_ = -2;
    return;
  }
  bool use_efficient_uuids = this->supports_efficient_uuids_();

  api::BluetoothGATTGetServicesResponse resp;
  resp.address = this->address_;
  static constexpr size_t MAX_PACKET_SIZE = 1360;
  size_t current_size = resp.calculate_size();

  while ((size_t) this->send_service_ < this->services_.size()) {
    auto *svc = this->services_[this->send_service_];
    // Stop the batch if this service likely won't fit (unless it's the first).
    size_t estimated = 25 + (size_t) svc->characteristics.size() * 60;
    if (!resp.services.empty() && current_size + estimated > MAX_PACKET_SIZE)
      break;

    resp.services.emplace_back();
    auto &service_resp = resp.services.back();
    fill_gatt_uuid(service_resp.uuid, service_resp.short_uuid, svc->uuid.get_uuid(), use_efficient_uuids);
    service_resp.handle = svc->start_handle;
    service_resp.characteristics.init(svc->characteristics.size());
    for (auto *chr : svc->characteristics) {
      service_resp.characteristics.emplace_back();
      auto &cr = service_resp.characteristics.back();
      fill_gatt_uuid(cr.uuid, cr.short_uuid, chr->uuid.get_uuid(), use_efficient_uuids);
      cr.handle = chr->handle;
      cr.properties = chr->properties;
      cr.descriptors.init(chr->descriptors.size());
      for (auto *d : chr->descriptors) {
        cr.descriptors.emplace_back();
        auto &dr = cr.descriptors.back();
        fill_gatt_uuid(dr.uuid, dr.short_uuid, d->uuid.get_uuid(), use_efficient_uuids);
        dr.handle = d->handle;
      }
    }
    current_size += service_resp.calculate_size() + 1;
    this->send_service_++;
  }

  api_conn->send_message(resp);
}

}  // namespace bluetooth_proxy
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
