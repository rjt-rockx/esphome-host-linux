#pragma once

#ifdef USE_HOST

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

#include <string>

namespace esphome {
namespace linux_sysfs_sensor {

// Reads a single numeric value from a sysfs file (e.g. a hwmon/thermal/IIO
// attribute) on each poll and publishes it as raw * scale. The exact path and
// scale are supplied explicitly by the user: sysfs indices (hwmonN, iio:deviceN)
// are assigned by probe order and are not stable across boots, so there is no
// auto-discovery -- see the component docs for pinning by the chip `name`.
class LinuxSysfsSensor : public sensor::Sensor, public PollingComponent {
 public:
  void set_path(std::string path) { this->path_ = std::move(path); }
  void set_scale(float scale) { this->scale_ = scale; }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  std::string path_;
  float scale_{1.0f};
};

}  // namespace linux_sysfs_sensor
}  // namespace esphome

#endif  // USE_HOST
