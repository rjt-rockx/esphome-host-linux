#pragma once
#ifdef USE_HOST

// Server-side events marshalled from the BLEGattServer sd-bus worker thread to the
// ESPHome main thread. BlueZ invokes the vtable handlers on the worker thread;
// those handlers update the value store under a lock (so reads can be answered
// synchronously) and post these events to the main thread, where BLEServer routes
// them to the owning characteristic's callbacks and the server connect/disconnect
// callbacks.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace esp32_ble_server {

struct HostGattServerEvent {
  enum class Kind : uint8_t {
    CONNECT,        // a remote central connected (conn_id minted from device path)
    DISCONNECT,     // a remote central disconnected
    WRITE,          // WriteValue on a characteristic/descriptor (data + conn_id)
    READ,           // ReadValue fired (advisory; value already served from store)
    SUBSCRIBE,      // StartNotify on a characteristic (conn_id subscribed)
    UNSUBSCRIBE,    // StopNotify on a characteristic (conn_id unsubscribed)
    REGISTERED,     // GattManager1.RegisterApplication succeeded
    REGISTER_FAILED,  // RegisterApplication failed (status carries -errno)
  } kind;

  // Identifies the target attribute for WRITE/READ/SUBSCRIBE/UNSUBSCRIBE. The
  // worker assigns each created characteristic/descriptor a stable uint16_t
  // handle (its own counter; BlueZ's real Handle is read back separately for the
  // API's get_handle()). 0 for connection-level events.
  uint16_t handle{0};
  bool is_desc{false};  // WRITE/READ target is a descriptor (vs characteristic)

  // A synthetic, stable per-connection id minted from the BlueZ device path.
  // 0 = unknown/not connection-scoped.
  uint16_t conn_id{0};

  // Payload for WRITE (owned copy taken off the sd_bus_message on the worker).
  std::unique_ptr<uint8_t[]> data;
  uint16_t len{0};

  int status{0};  // REGISTER_FAILED: -errno; otherwise 0
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
