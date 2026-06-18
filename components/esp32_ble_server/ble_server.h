#pragma once

#include "ble_service.h"
#include "ble_characteristic.h"

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <functional>
#include <memory>
#include <vector>

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// Host shadow of esp32_ble_server's BLEServer. Upstream this drives the IDF gatts
// app-register → create-service → start-service state machine; on host the whole
// tree is built at codegen time and exported in one GattManager1.RegisterApplication
// by the BLEGattServer. setup() builds that server, registers it as the parent
// ESP32BLE's AdvertisingBackend, and kicks registration; loop() drains
// connect/disconnect events the server posts and dispatches on_connect/on_disconnect.

namespace esphome {
namespace esp32_ble_server {

using namespace esp32_ble;
using namespace bytebuffer;

class BLEGattServer;

class BLEServer : public Component, public Parented<ESP32BLE> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;
  bool can_proceed() override;

  bool is_running() { return this->state_ == RUNNING; }

  void set_manufacturer_data(const std::vector<uint8_t> &data) {
    this->manufacturer_data_ = data;
    this->restart_advertising_();
  }

  void set_max_clients(uint8_t max_clients) { this->max_clients_ = max_clients; }
  uint8_t get_max_clients() const { return this->max_clients_; }

  BLEService *create_service(ESPBTUUID uuid, bool advertise = false, uint16_t num_handles = 15);
  void remove_service(ESPBTUUID uuid, uint8_t inst_id = 0);
  BLEService *get_service(ESPBTUUID uuid, uint8_t inst_id = 0);
  void enqueue_start_service(BLEService *service) { this->services_to_start_.push_back(service); }
  void set_device_information_service(BLEService *service) { this->device_information_service_ = service; }

  // Stub IDF accessors kept for source-compat with anything that names them.
  int get_gatts_if() { return 0; }
  uint32_t get_connected_client_count() { return this->clients_.size(); }
  const uint16_t *get_clients() const { return this->clients_.data(); }
  uint8_t get_client_count() const { return static_cast<uint8_t>(this->clients_.size()); }

  void ble_before_disabled_event_handler() {}

  // Direct callback registration - supports multiple callbacks.
  void on_connect(std::function<void(uint16_t)> &&callback) {
    this->callbacks_.push_back({CallbackType::ON_CONNECT, std::move(callback)});
  }
  void on_disconnect(std::function<void(uint16_t)> &&callback) {
    this->callbacks_.push_back({CallbackType::ON_DISCONNECT, std::move(callback)});
  }

  // Called (main thread, from loop()) when BLEGattServer drained a connect/
  // disconnect for a central. Tracks the client and dispatches callbacks.
  void host_on_connect(uint16_t conn_id);
  void host_on_disconnect(uint16_t conn_id);

 protected:
  enum class CallbackType : uint8_t {
    ON_CONNECT,
    ON_DISCONNECT,
  };

  struct CallbackEntry {
    CallbackType type;
    std::function<void(uint16_t)> callback;
  };

  struct ServiceEntry {
    ESPBTUUID uuid;
    uint8_t inst_id;
    BLEService *service;
  };

  void restart_advertising_();
  void dispatch_callbacks_(CallbackType type, uint16_t conn_id);

  std::vector<CallbackEntry> callbacks_;

  std::vector<uint8_t> manufacturer_data_{};

  std::vector<uint16_t> clients_;  // connected centrals (synthetic conn_ids)
  uint8_t max_clients_{1};
  std::vector<ServiceEntry> services_{};
  std::vector<BLEService *> services_to_start_{};
  BLEService *device_information_service_{nullptr};

  std::unique_ptr<BLEGattServer> gatt_server_;

  enum State : uint8_t {
    INIT = 0x00,
    REGISTERING,
    STARTING_SERVICE,
    RUNNING,
  } state_{INIT};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern BLEServer *global_ble_server;

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
