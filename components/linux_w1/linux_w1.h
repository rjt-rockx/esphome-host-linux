#pragma once

#ifdef USE_HOST

#include <string>

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace linux_w1 {

class LinuxW1Sensor : public sensor::Sensor, public PollingComponent {
 public:
  void set_address(std::string address) { address_ = std::move(address); }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  std::string address_;
  std::string sysfs_path_;
  bool path_ok_{false};
};

}  // namespace linux_w1
}  // namespace esphome

#endif  // USE_HOST
