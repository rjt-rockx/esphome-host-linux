#pragma once

#ifdef USE_HOST

#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"

namespace esphome {
namespace linux_time {

// Host platform already has its clock kept in sync by the OS (systemd-timesyncd
// / chrony / ntpd). Nothing to do at runtime: RealTimeClock::timestamp_now()
// calls ::time(nullptr), which reads the kernel's notion of wall time.
class LinuxTime : public time::RealTimeClock {
 public:
  void setup() override;
  void update() override {}
  void dump_config() override;
  float get_setup_priority() const override;
};

}  // namespace linux_time
}  // namespace esphome

#endif  // USE_HOST
