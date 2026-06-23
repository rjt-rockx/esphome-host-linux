#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_client.h"

#include "esphome/core/log.h"

namespace esphome {
namespace ble_client {

static const char *const TAG = "ble_client";

void BLEClient::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Client:");
  ESP_LOGCONFIG(TAG, "  Address: %s", this->address_str());
}

void BLEClient::set_enabled(bool enabled) {
  if (enabled == this->enabled)
    return;
  this->enabled = enabled;
  if (!enabled && this->state() != espbt::ClientState::IDLE) {
    this->disconnect();
  }
}

bool BLEClient::all_nodes_established_() {
  if (this->state() != espbt::ClientState::ESTABLISHED)
    return false;
  for (auto *node : this->nodes_)
    if (node->node_state != espbt::ClientState::ESTABLISHED)
      return false;
  return true;
}

// Fan one event to all nodes. set_state() (which fans node_state) runs in the
// base dispatch BEFORE the hooks here, so a hook reading its own node_state sees
// the new value. The release_services()/established check runs once per batch in
// loop(), never mid-batch.
void BLEClient::dispatch_event_(const HostGattEvent &ev) {
  // Advance the base state machine + fan node_state first.
  BLEClientBase::dispatch_event_(ev);
  for (auto *node : this->nodes_)
    node->node_state = this->state();

  switch (ev.kind) {
    case HostGattEvent::Kind::CONNECTED:
      for (auto *n : this->nodes_)
        n->on_connected();
      break;
    case HostGattEvent::Kind::SERVICES_DISCOVERED:
      for (auto *n : this->nodes_)
        n->on_services_discovered();
      break;
    case HostGattEvent::Kind::DISCONNECTED:
      for (auto *n : this->nodes_)
        n->on_disconnected(ev.disc_reason);
      break;
    case HostGattEvent::Kind::READ_COMPLETE: {
      BLEReadResult r{ev.handle, ev.status, ev.data.get(), ev.len};
      for (auto *n : this->nodes_)
        n->on_characteristic_read(r);
      break;
    }
    case HostGattEvent::Kind::DESC_READ: {
      BLEReadResult r{ev.handle, ev.status, ev.data.get(), ev.len};
      for (auto *n : this->nodes_)
        n->on_descriptor_read(r);
      break;
    }
    case HostGattEvent::Kind::WRITE_COMPLETE:
      for (auto *n : this->nodes_)
        n->on_characteristic_write(ev.handle, ev.status);
      break;
    case HostGattEvent::Kind::DESC_WRITE:
      for (auto *n : this->nodes_)
        n->on_descriptor_write(ev.handle, ev.status);
      break;
    case HostGattEvent::Kind::NOTIFY: {
      BLENotifyEvent e{ev.handle, ev.data.get(), ev.len};
      for (auto *n : this->nodes_)
        n->on_notify(e);
      break;
    }
    case HostGattEvent::Kind::NOTIFY_REGISTERED:
      for (auto *n : this->nodes_)
        n->on_notify_registered(ev.handle, ev.status);
      break;
    case HostGattEvent::Kind::RSSI:
      for (auto *n : this->nodes_)
        n->on_rssi(ev.rssi);
      break;
    case HostGattEvent::Kind::PASSKEY_REQUEST:
      for (auto *n : this->nodes_)
        n->on_passkey_request();
      break;
    case HostGattEvent::Kind::PASSKEY_NOTIFY:
      for (auto *n : this->nodes_)
        n->on_passkey_notification(ev.passkey);
      break;
    case HostGattEvent::Kind::NUMERIC_COMPARE:
      for (auto *n : this->nodes_)
        n->on_numeric_comparison_request(ev.passkey);
      break;
    case HostGattEvent::Kind::PAIRING_COMPLETE:
      for (auto *n : this->nodes_)
        n->on_pairing_complete(ev.pairing_success, ev.disc_reason);
      break;
  }
}

void BLEClient::loop() {
  // Drain + dispatch worker events first, then run each node's loop hook.
  // Nodes may rely on seeing dispatched events before their own loop() runs.
  BLEClientBase::loop();
  for (auto *node : this->nodes_)
    node->loop();
}

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
