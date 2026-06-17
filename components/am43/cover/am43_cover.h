#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/am43/am43_base.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/esp32_ble_client/host_gatt_event.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"

#include <memory>

namespace esphome {
namespace am43 {

namespace espbt = esphome::esp32_ble_tracker;
using esphome::esp32_ble_client::BLENotifyEvent;

class Am43Component : public cover::Cover, public esphome::ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  cover::CoverTraits get_traits() override;
  void set_pin(uint16_t pin) { this->pin_ = pin; }
  void set_invert_position(bool invert_position) { this->invert_position_ = invert_position; }

  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_notify(const BLENotifyEvent &e) override;

 protected:
  void control(const cover::CoverCall &call) override;
  void write_packet_(Am43Packet *packet);

  uint16_t char_handle_{0};
  uint16_t pin_{0};
  bool invert_position_{false};
  std::unique_ptr<Am43Encoder> encoder_;
  std::unique_ptr<Am43Decoder> decoder_;
  bool logged_in_{false};
  float position_{0};
};

}  // namespace am43
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
