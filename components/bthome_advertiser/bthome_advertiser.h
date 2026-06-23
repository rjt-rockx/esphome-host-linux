#pragma once

#ifdef USE_HOST

#include "bthome_encoder.h"

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_host_thread.h"
#include "esphome/core/component.h"

#include <cstdint>
#include <mutex>
#include <vector>

#include <systemd/sd-bus.h>

namespace esphome {
namespace bthome_advertiser {

// Host (Linux/BlueZ) BTHome v2 advertiser. Registers its own org.bluez
// LEAdvertisement1 (Type="broadcast") carrying a Service Data AD for UUID 0xFCD2
// with a BTHome v2 payload, on the shared esp32_ble::BleWorker event loop. A
// distinct object path lets it coexist with other advertisements. The Linux box
// publishes its own readings as BTHome for Home Assistant / other receivers.
static constexpr const char *BTHOME_ADV_PATH = "/org/esphome/host/ble/bthome/advertisement0";

class BTHomeAdvertiser : public Component, public esp32_ble::BusWorkerClient {
 public:
  ~BTHomeAdvertiser() override;

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // Codegen (to_code) appends one entry per configured measurement.
  void add_measurement(uint8_t object_id, double value) { this->measurements_.push_back({object_id, value}); }
  void set_encrypted(bool encrypted) { this->encrypted_ = encrypted; }

  // BusWorkerClient — drain the main→worker start request (worker thread).
  void worker_process_commands() override;

  // --- LEAdvertisement1 vtable trampolines (invoked only by sd-bus) ---
  static int adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
  static int adv_get_service_uuids_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                    sd_bus_error *);
  static int adv_get_service_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                   sd_bus_error *);
  static int adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                               sd_bus_error *);
  static int adv_release_(sd_bus_message *, void *, sd_bus_error *);

 protected:
  bool open_bus_();
  void do_start_();
  void register_advertisement_();
  void unregister_advertisement_();
  static int on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);

  std::vector<BTHomeObject> measurements_;
  bool encrypted_{false};
  std::vector<uint8_t> payload_;  // built once in setup(); read by the adv getter

  sd_bus *bus_{nullptr};
  sd_bus_slot *adv_slot_{nullptr};
  bool bus_open_{false};
  bool started_{false};          // worker-only: do_start_ already ran
  bool start_requested_{false};  // main→worker: START queued
  bool adv_registered_{false};
  bool attached_{false};
  std::mutex mu_;
};

}  // namespace bthome_advertiser
}  // namespace esphome

#endif  // USE_HOST
