#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_client/host_gatt_event.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"

#include <vector>

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;
using namespace esp32_ble_client;

class BLEClient;

// A node attached to a BLEClient connection. Exposes discrete, default-no-op
// hooks for GATT/pairing events fanned out by the parent BLEClient.
class BLEClientNode {
 public:
  virtual void on_connected() {}
  virtual void on_disconnected(int reason) {}
  virtual void on_services_discovered() {}
  virtual void on_characteristic_read(const BLEReadResult &r) {}
  virtual void on_characteristic_write(uint16_t handle, int status) {}
  virtual void on_descriptor_read(const BLEReadResult &r) {}
  virtual void on_descriptor_write(uint16_t handle, int status) {}
  virtual void on_notify(const BLENotifyEvent &e) {}
  virtual void on_notify_registered(uint16_t handle, int status) {}
  virtual void on_rssi(int8_t rssi) {}
  virtual void on_passkey_request() {}
  virtual void on_passkey_notification(uint32_t passkey) {}
  virtual void on_numeric_comparison_request(uint32_t passkey) {}
  virtual void on_pairing_complete(bool success, int reason) {}
  virtual void loop() {}

  void set_address(uint64_t address) { this->address_ = address; }
  espbt::ClientState node_state{espbt::ClientState::INIT};
  BLEClient *parent() { return this->parent_; }
  void set_ble_client_parent(BLEClient *parent) { this->parent_ = parent; }

 protected:
  uint64_t address_{0};
  BLEClient *parent_{nullptr};
};

class BLEClient : public BLEClientBase {
 public:
  void loop() override;
  void dump_config() override;

  void set_enabled(bool enabled);
  bool enabled{true};

  void register_ble_node(BLEClientNode *node) {
    node->set_ble_client_parent(this);
    this->nodes_.push_back(node);
  }

 protected:
  void dispatch_event_(const HostGattEvent &ev) override;
  bool all_nodes_established_();

  std::vector<BLEClientNode *> nodes_;
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
