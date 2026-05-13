#pragma once

#ifdef USE_HOST

#include <string>

#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/component.h"

namespace esphome {
namespace linux_i2c {

class LinuxI2CBus : public i2c::I2CBus, public Component {
 public:
  void set_device(std::string path) { device_path_ = std::move(path); }
  void set_scan(bool scan) { this->scan_ = scan; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  i2c::ErrorCode write_readv(uint8_t address, const uint8_t *write_buffer, size_t write_count, uint8_t *read_buffer,
                             size_t read_count) override;

 protected:
  std::string device_path_;
  int fd_{-1};
};

}  // namespace linux_i2c
}  // namespace esphome

#endif  // USE_HOST
