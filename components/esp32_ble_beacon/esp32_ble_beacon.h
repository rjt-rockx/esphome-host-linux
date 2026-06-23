#pragma once

#ifdef USE_HOST

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_host_thread.h"
#include "esphome/core/component.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include <systemd/sd-bus.h>

namespace esphome {
namespace esp32_ble_beacon {

// Host (Linux/BlueZ) iBeacon advertiser. Registers its own org.bluez
// LEAdvertisement1 (Type="broadcast") carrying the Apple-iBeacon ManufacturerData
// (company 0x004C) on the shared esp32_ble::BleWorker event loop, independent of
// the GATT server's advertisement; a distinct object path (BEACON_ADV_PATH) lets
// the two coexist. The on-air iBeacon value is 0x02 0x15 prefix + 16-byte
// proximity UUID + major/minor big-endian + measured power. BlueZ prepends the AD
// length/type/company-id and the Flags AD, so only the value is provided here.
static constexpr const char *BEACON_ADV_PATH = "/org/esphome/host/ble/beacon/advertisement0";

class ESP32BLEBeacon : public Component, public esp32_ble::BusWorkerClient {
 public:
  explicit ESP32BLEBeacon(const std::array<uint8_t, 16> &uuid) : uuid_(uuid) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_major(uint16_t major) { this->major_ = major; }
  void set_minor(uint16_t minor) { this->minor_ = minor; }
  void set_min_interval(uint16_t val) { this->min_interval_ = val; }
  void set_max_interval(uint16_t val) { this->max_interval_ = val; }
  void set_measured_power(int8_t val) { this->measured_power_ = val; }
  void set_tx_power(int8_t val) { this->tx_power_ = val; }

  // BusWorkerClient — drain the main→worker start request (worker thread).
  void worker_process_commands() override;

  // --- LEAdvertisement1 vtable trampolines ---
  // Public because the file-scope sd_bus_vtable array in the .cpp names them.
  // They take a void* userdata and are only ever invoked by sd-bus itself.
  static int adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
  static int adv_get_manufacturer_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                        sd_bus_error *);
  static int adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                               sd_bus_error *);
  static int adv_release_(sd_bus_message *, void *, sd_bus_error *);

 protected:
  bool open_bus_();
  // Worker-thread bring-up: open the bus, export the LEAdvertisement1, and fire
  // RegisterAdvertisement asynchronously (BlueZ reads our properties back before
  // replying, so a blocking call would starve the worker loop).
  void do_start_();
  void register_advertisement_();
  static int on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
  std::vector<uint8_t> build_ibeacon_payload_() const;

  std::array<uint8_t, 16> uuid_;
  uint16_t major_{};
  uint16_t minor_{};
  uint16_t min_interval_{};
  uint16_t max_interval_{};
  int8_t measured_power_{};
  int8_t tx_power_{};

  std::vector<uint8_t> mfg_data_;  // built once in setup(); read by the worker

  sd_bus *bus_{nullptr};
  sd_bus_slot *adv_slot_{nullptr};
  bool bus_open_{false};
  bool started_{false};         // worker-only: do_start_ already ran
  bool start_requested_{false};  // main→worker: START queued
  bool adv_registered_{false};
  std::mutex mu_;
};

}  // namespace esp32_ble_beacon
}  // namespace esphome

#endif  // USE_HOST
