#pragma once
#ifdef USE_HOST

// BleWorker — the shared sd-event worker thread for all host BLE D-Bus work.
// One thread, one sd_event loop, services every attached BusWorkerClient's bus.
// Both the GATT client (esp32_ble_client::BLEGattHost) and the GATT server
// (esp32_ble_server::BLEGattServer) attach their own sd_bus to event() and
// register as BusWorkerClients so their command queues are drained once per loop
// pass. Created lazily on first use; the singleton outlives all clients.
//
// Lives in esp32_ble (the BLE base both components already auto-load) so the
// server does not have to depend on the whole GATT *client* component just to
// share the loop.

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct sd_event;
struct sd_event_source;

namespace esphome {
namespace esp32_ble {

// Anything that owns an sd_bus attached to the shared worker loop and needs its
// command queue drained once per loop iteration implements this.
struct BusWorkerClient {
  virtual ~BusWorkerClient() = default;
  // Run on the worker thread after each sd_event_run return: drain the client's
  // main-thread→worker command queue and dispatch to D-Bus.
  virtual void worker_process_commands() = 0;
};

class BleWorker {
 public:
  static BleWorker &instance();

  // Register/unregister a client so the worker drains its command queue. Starts
  // the worker thread lazily on first attach.
  void attach_worker_client(BusWorkerClient *client);
  void detach_worker_client(BusWorkerClient *client);

  // Wake the event loop to process newly-queued commands.
  void wake();
  sd_event *event() { return this->event_; }

 protected:
  BleWorker() = default;
  void ensure_started_();
  void run_();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  sd_event *event_{nullptr};
  sd_event_source *wake_source_{nullptr};
  int wake_fd_{-1};  // eventfd to wake the loop from other threads
  std::mutex clients_mu_;
  std::vector<BusWorkerClient *> worker_clients_;
};

}  // namespace esp32_ble
}  // namespace esphome

#endif  // USE_HOST
