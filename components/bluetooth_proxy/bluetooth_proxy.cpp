#include "bluetooth_proxy.h"

#include "esphome/components/api/api_server.h"
#include "esphome/core/log.h"
#include "esphome/core/macros.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cstring>
#include <limits>

#if defined(USE_ESP32) || defined(USE_HOST)

namespace esphome::bluetooth_proxy {

static const char *const TAG = "bluetooth_proxy";

// BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE is defined during code generation
// It sets the batch size for BLE advertisements to maximize WiFi efficiency

// Verify BLE advertisement data array size matches the BLE specification (31 bytes adv + 31 bytes scan response)
static_assert(sizeof(((api::BluetoothLERawAdvertisement *) nullptr)->data) == 62,
              "BLE advertisement data array size mismatch");

BluetoothProxy::BluetoothProxy() { global_bluetooth_proxy = this; }

void BluetoothProxy::setup() {
#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  this->connections_free_response_.limit = BLUETOOTH_PROXY_MAX_CONNECTIONS;
  this->connections_free_response_.free = BLUETOOTH_PROXY_MAX_CONNECTIONS;
#endif

  // Capture the configured scan mode from YAML before any API changes
  this->configured_scan_active_ = this->hub_->get_scan_active();

  this->hub_->add_scanner_state_listener(this);
  this->hub_->set_raw_advertisement_callback({this, [](void *self, const ble_device_base::RawAdvertisement &adv) {
                                                static_cast<BluetoothProxy *>(self)->on_raw_advertisement_(adv);
                                              }});
}

void BluetoothProxy::on_scanner_state(esp32_ble_tracker::ScannerState state) {
  if (this->api_connection_ != nullptr) {
    this->send_bluetooth_scanner_state_(state);
  }
}

void BluetoothProxy::send_bluetooth_scanner_state_(esp32_ble_tracker::ScannerState state) {
  api::BluetoothScannerStateResponse resp;
  resp.state = static_cast<api::enums::BluetoothScannerState>(state);
  resp.mode = this->hub_->get_scan_active() ? api::enums::BluetoothScannerMode::BLUETOOTH_SCANNER_MODE_ACTIVE
                                            : api::enums::BluetoothScannerMode::BLUETOOTH_SCANNER_MODE_PASSIVE;
  resp.configured_mode = this->configured_scan_active_
                             ? api::enums::BluetoothScannerMode::BLUETOOTH_SCANNER_MODE_ACTIVE
                             : api::enums::BluetoothScannerMode::BLUETOOTH_SCANNER_MODE_PASSIVE;
  [[maybe_unused]] bool sent = this->api_connection_->send_message(resp);
}

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
void BluetoothProxy::log_connection_request_ignored_(BluetoothConnection *connection, espbt::ClientState state) {
  ESP_LOGW(TAG, "[%d] [%s] Connection request ignored, state: %s", connection->get_connection_index(),
           connection->address_str(), espbt::client_state_to_string(state));
}

void BluetoothProxy::log_connection_info_(BluetoothConnection *connection, const char *message) {
  ESP_LOGI(TAG, "[%d] [%s] Connecting %s", connection->get_connection_index(), connection->address_str(), message);
}

void BluetoothProxy::log_not_connected_gatt_(const char *action, const char *type) {
  ESP_LOGW(TAG, "Cannot %s GATT %s, not connected", action, type);
}

void BluetoothProxy::handle_gatt_not_connected_(uint64_t address, uint16_t handle, const char *action,
                                                const char *type) {
  this->log_not_connected_gatt_(action, type);
  this->send_gatt_error(address, handle, ESP_GATT_NOT_CONNECTED);
}
#endif  // USE_BLUETOOTH_PROXY_CONNECTIONS

// The hub delivers advertisements on the ESPHome main loop, already in raw AD
// form (the BlueZ backend re-encodes its parsed properties there).
void BluetoothProxy::on_raw_advertisement_(const ble_device_base::RawAdvertisement &raw) {
  if (!api::global_api_server->is_connected() || this->api_connection_ == nullptr)
    return;

  auto &adv = this->response_.advertisements[this->response_.advertisements_len];
  adv.address = raw.address;
  adv.rssi = raw.rssi;
  adv.address_type = raw.addr_type;
  uint8_t length = raw.data_len > sizeof(adv.data) ? sizeof(adv.data) : static_cast<uint8_t>(raw.data_len);
  adv.data_len = length;
  std::memcpy(adv.data, raw.data, length);
  this->response_.advertisements_len++;

  if (this->response_.advertisements_len >= BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE)
    this->flush_pending_advertisements_();
}

void BluetoothProxy::log_advertisement_flush_() {
  ESP_LOGV(TAG, "Sent batch of %u BLE advertisements", this->response_.advertisements_len);
}

void BluetoothProxy::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Bluetooth Proxy:\n"
                "  Active: %s\n"
                "  Connections: %d",
                YESNO(this->active_), BLUETOOTH_PROXY_MAX_CONNECTIONS);
}

void BluetoothProxy::loop() {
  // Run advertisement flush / connection cleanup every 100ms
  uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_advertisement_flush_time_ < 100)
    return;
  this->last_advertisement_flush_time_ = now;

  if (api::global_api_server->is_connected() && this->api_connection_ != nullptr) {
    this->flush_pending_advertisements_();
    return;
  }
#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
  // No API client left to answer: drop the connections it was holding.
  for (uint8_t i = 0; i < this->connection_count_; i++) {
    auto *connection = this->connections_[i];
    if (connection->get_address() != 0 && !connection->disconnect_pending()) {
      connection->disconnect();
    }
  }
#endif
}

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
BluetoothConnection *BluetoothProxy::get_connection_(uint64_t address, bool reserve) {
  for (uint8_t i = 0; i < this->connection_count_; i++) {
    auto *connection = this->connections_[i];
    uint64_t conn_addr = connection->get_address();

    if (conn_addr == address)
      return connection;

    if (reserve && conn_addr == 0) {
      connection->send_service_ = INIT_SENDING_SERVICES;
      connection->set_address(address);
      // All connections must start at INIT
      // We only set the state if we allocate the connection
      // to avoid a race where multiple connection attempts
      // are made.
      connection->set_state(espbt::ClientState::INIT);
      return connection;
    }
  }
  return nullptr;
}

void BluetoothProxy::bluetooth_device_request(const api::BluetoothDeviceRequest &msg) {
  switch (msg.request_type) {
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITH_CACHE:
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE: {
      auto *connection = this->get_connection_(msg.address, true);
      if (connection == nullptr) {
        ESP_LOGW(TAG, "No free connections available");
        this->send_device_connection(msg.address, false);
        return;
      }
      if (!msg.has_address_type) {
        ESP_LOGE(TAG, "[%d] [%s] Missing address type in connect request", connection->get_connection_index(),
                 connection->address_str());
        this->send_device_connection(msg.address, false);
        return;
      }
      if (connection->state() == espbt::ClientState::CONNECTED ||
          connection->state() == espbt::ClientState::ESTABLISHED) {
        this->log_connection_request_ignored_(connection, connection->state());
        this->send_device_connection(msg.address, true);
        this->send_connections_free();
        return;
      } else if (connection->state() == espbt::ClientState::CONNECTING) {
        if (connection->disconnect_pending()) {
          ESP_LOGW(TAG, "[%d] [%s] Connection request while pending disconnect, cancelling pending disconnect",
                   connection->get_connection_index(), connection->address_str());
          connection->cancel_pending_disconnect();
          return;
        }
        this->log_connection_request_ignored_(connection, connection->state());
        return;
      } else if (connection->state() != espbt::ClientState::INIT) {
        this->log_connection_request_ignored_(connection, connection->state());
        return;
      }
      if (msg.request_type == api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITH_CACHE) {
        connection->set_connection_type(espbt::ConnectionType::V3_WITH_CACHE);
        this->log_connection_info_(connection, "v3 with cache");
      } else {  // BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE
        connection->set_connection_type(espbt::ConnectionType::V3_WITHOUT_CACHE);
        this->log_connection_info_(connection, "v3 without cache");
      }
      // Address type is irrelevant on host (BlueZ resolves the device by its
      // object path, derived from the MAC).
      connection->set_state(espbt::ClientState::DISCOVERED);
      this->send_connections_free();
      break;
    }
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_DISCONNECT: {
      auto *connection = this->get_connection_(msg.address, false);
      if (connection == nullptr) {
        this->send_device_connection(msg.address, false);
        this->send_connections_free();
        return;
      }
      if (connection->state() != espbt::ClientState::IDLE) {
        connection->disconnect();
      } else {
        connection->set_address(0);
        this->send_device_connection(msg.address, false);
        this->send_connections_free();
      }
      break;
    }
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR: {
      auto *connection = this->get_connection_(msg.address, false);
      if (connection != nullptr) {
        if (!connection->is_paired()) {
          auto err = connection->pair();
          if (err != ESP_OK) {
            this->send_device_pairing(msg.address, false, err);
          }
        } else {
          this->send_device_pairing(msg.address, true);
        }
      }
      break;
    }
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR: {
      // On host, removing the bond is RemoveDevice on the adapter — route via the
      // connection if one exists for this address.
      auto *connection = this->get_connection_(msg.address, false);
      if (connection != nullptr) {
        connection->remove_bond();
        this->send_device_pairing(msg.address, false);
      } else {
        this->send_device_pairing(msg.address, false, ESP_GATT_NOT_FOUND);
      }
      break;
    }
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE: {
      // BlueZ manages its own GATT cache; there is no per-request clean. Report
      // success (the next connection re-resolves services from BlueZ anyway).
      api::BluetoothDeviceClearCacheResponse call;
      call.address = msg.address;
      call.success = true;
      call.error = ESP_OK;
      [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
      break;
    }
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT: {
      ESP_LOGE(TAG, "V1 connections removed");
      this->send_device_connection(msg.address, false);
      break;
    }
  }
}

void BluetoothProxy::bluetooth_gatt_read(const api::BluetoothGATTReadRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr) {
    this->handle_gatt_not_connected_(msg.address, msg.handle, "read", "characteristic");
    return;
  }

  auto err = connection->read_characteristic(msg.handle);
  if (err != ESP_OK) {
    this->send_gatt_error(msg.address, msg.handle, err);
  }
}

void BluetoothProxy::bluetooth_gatt_write(const api::BluetoothGATTWriteRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr) {
    this->handle_gatt_not_connected_(msg.address, msg.handle, "write", "characteristic");
    return;
  }

  auto err = connection->write_characteristic(msg.handle, msg.data, msg.data_len, msg.response);
  if (err != ESP_OK) {
    this->send_gatt_error(msg.address, msg.handle, err);
  }
}

void BluetoothProxy::bluetooth_gatt_read_descriptor(const api::BluetoothGATTReadDescriptorRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr) {
    this->handle_gatt_not_connected_(msg.address, msg.handle, "read", "descriptor");
    return;
  }

  auto err = connection->read_descriptor(msg.handle);
  if (err != ESP_OK) {
    this->send_gatt_error(msg.address, msg.handle, err);
  }
}

void BluetoothProxy::bluetooth_gatt_write_descriptor(const api::BluetoothGATTWriteDescriptorRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr) {
    this->handle_gatt_not_connected_(msg.address, msg.handle, "write", "descriptor");
    return;
  }

  auto err = connection->write_descriptor(msg.handle, msg.data, msg.data_len, true);
  if (err != ESP_OK) {
    this->send_gatt_error(msg.address, msg.handle, err);
  }
}

void BluetoothProxy::bluetooth_gatt_send_services(const api::BluetoothGATTGetServicesRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr || !connection->connected()) {
    this->handle_gatt_not_connected_(msg.address, 0, "get", "services");
    return;
  }
  if (connection->service_count() == 0) {
    ESP_LOGW(TAG, "[%d] [%s] No GATT services found", connection->connection_index_, connection->address_str());
    this->send_gatt_services_done(msg.address);
    return;
  }
  if (connection->send_service_ == INIT_SENDING_SERVICES)  // Start sending services if not started yet
    connection->send_service_ = 0;
}

void BluetoothProxy::bluetooth_gatt_notify(const api::BluetoothGATTNotifyRequest &msg) {
  auto *connection = this->get_connection_(msg.address, false);
  if (connection == nullptr) {
    this->handle_gatt_not_connected_(msg.address, msg.handle, "notify", "characteristic");
    return;
  }

  auto err = connection->notify_characteristic(msg.handle, msg.enable);
  if (err != ESP_OK) {
    this->send_gatt_error(msg.address, msg.handle, err);
  }
}

void BluetoothProxy::bluetooth_set_connection_params(const api::BluetoothSetConnectionParamsRequest &msg) {
  if (this->api_connection_ == nullptr)
    return;

  auto *connection = this->get_connection_(msg.address, false);
  api::BluetoothSetConnectionParamsResponse resp;
  resp.address = msg.address;

  if (connection == nullptr || !connection->connected()) {
    ESP_LOGW(TAG, "[%d] [%s] Cannot set connection params, not connected",
             connection ? static_cast<int>(connection->connection_index_) : -1,
             connection ? connection->address_str() : "unknown");
    resp.error = ESP_GATT_NOT_CONNECTED;
    [[maybe_unused]] bool sent = this->api_connection_->send_message(resp);
    return;
  }

  // Protobuf fields are uint32_t to future-proof the API if BLE ever supports wider values;
  // clamp to uint16_t since the current BLE spec defines these as 16-bit.
  constexpr uint32_t max_val = std::numeric_limits<uint16_t>::max();
  resp.error = connection->update_connection_params(static_cast<uint16_t>(std::min(msg.min_interval, max_val)),
                                                    static_cast<uint16_t>(std::min(msg.max_interval, max_val)),
                                                    static_cast<uint16_t>(std::min(msg.latency, max_val)),
                                                    static_cast<uint16_t>(std::min(msg.timeout, max_val)));
  [[maybe_unused]] bool sent = this->api_connection_->send_message(resp);
}
#endif  // USE_BLUETOOTH_PROXY_CONNECTIONS

void BluetoothProxy::subscribe_api_connection(api::APIConnection *api_connection, uint32_t flags) {
  if (this->api_connection_ != nullptr) {
    ESP_LOGE(TAG, "Only one API subscription is allowed at a time");
    return;
  }
  this->api_connection_ = api_connection;
  // (No parser-type recalculation needed on host — parsed advertisements only.)
  this->send_bluetooth_scanner_state_(this->hub_->get_scanner_state());
}

void BluetoothProxy::unsubscribe_api_connection(api::APIConnection *api_connection) {
  if (this->api_connection_ != api_connection) {
    ESP_LOGV(TAG, "API connection is not subscribed");
    return;
  }
  this->api_connection_ = nullptr;
}

#ifdef USE_BLUETOOTH_PROXY_CONNECTIONS
void BluetoothProxy::send_device_connection(uint64_t address, bool connected, uint16_t mtu, esp_err_t error) {
  if (this->api_connection_ == nullptr)
    return;
  api::BluetoothDeviceConnectionResponse call;
  call.address = address;
  call.connected = connected;
  call.mtu = mtu;
  call.error = error;
  [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
}
void BluetoothProxy::send_connections_free() {
  if (this->api_connection_ != nullptr) {
    this->send_connections_free(this->api_connection_);
  }
}

void BluetoothProxy::send_connections_free(api::APIConnection *api_connection) {
  [[maybe_unused]] bool sent = api_connection->send_message(this->connections_free_response_);
}

void BluetoothProxy::send_gatt_services_done(uint64_t address) {
  if (this->api_connection_ == nullptr)
    return;
  api::BluetoothGATTGetServicesDoneResponse call;
  call.address = address;
  [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
}

void BluetoothProxy::send_gatt_error(uint64_t address, uint16_t handle, esp_err_t error) {
  if (this->api_connection_ == nullptr)
    return;
  api::BluetoothGATTErrorResponse call;
  call.address = address;
  call.handle = handle;
  call.error = error;
  [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
}

void BluetoothProxy::send_device_pairing(uint64_t address, bool paired, esp_err_t error) {
  if (this->api_connection_ == nullptr)
    return;
  api::BluetoothDevicePairingResponse call;
  call.address = address;
  call.paired = paired;
  call.error = error;

  [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
}

void BluetoothProxy::send_device_unpairing(uint64_t address, bool success, esp_err_t error) {
  if (this->api_connection_ == nullptr)
    return;
  api::BluetoothDeviceUnpairingResponse call;
  call.address = address;
  call.success = success;
  call.error = error;

  [[maybe_unused]] bool sent = this->api_connection_->send_message(call);
}
#endif  // USE_BLUETOOTH_PROXY_CONNECTIONS

void BluetoothProxy::bluetooth_scanner_set_mode(bool active) {
  // On the host D-Bus backend the scan is always active/continuous once started;
  // active/passive mode switching at runtime is a no-op (BlueZ manages it).
  ESP_LOGD(TAG, "Scanner mode request (%s) is a no-op on host", active ? "active" : "passive");
}

BluetoothProxy *global_bluetooth_proxy = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::bluetooth_proxy

#endif  // USE_ESP32
