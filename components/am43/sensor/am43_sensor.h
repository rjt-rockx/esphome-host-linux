#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/am43/am43_base.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_client/host_gatt_event.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

#include <memory>

namespace esphome {
namespace am43 {

namespace espbt = esphome::esp32_ble_tracker;
using esphome::esp32_ble_client::BLENotifyEvent;

class Am43 : public esphome::ble_client::BLEClientNode, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  void on_services_discovered() override;
  void on_disconnected(int reason) override;
  void on_notify(const BLENotifyEvent &e) override;

  void set_battery(sensor::Sensor *battery) { this->battery_ = battery; }
  void set_illuminance(sensor::Sensor *illuminance) { this->illuminance_ = illuminance; }

 protected:
  void write_packet_(Am43Packet *packet);
  uint16_t char_handle_{0};
  std::unique_ptr<Am43Encoder> encoder_;
  std::unique_ptr<Am43Decoder> decoder_;
  sensor::Sensor *battery_{nullptr};
  sensor::Sensor *illuminance_{nullptr};
  uint8_t current_sensor_{0};
  uint32_t last_battery_update_{0};
};

}  // namespace am43
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
