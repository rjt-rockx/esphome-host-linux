#ifdef USE_HOST

#include "linux_time.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace linux_time {

static const char *const TAG = "linux_time";

void LinuxTime::setup() {
  // OS already manages NTP. Just publish that "time is synced" so downstream
  // automations that wait on time.has_time fire on boot.
  this->time_sync_callback_.call();
  ESP_LOGI(TAG, "Using OS system clock (assume host clock is NTP-synced)");
}

void LinuxTime::dump_config() {
  ESP_LOGCONFIG(TAG, "Linux Time (host system clock):");
  time::RealTimeClock::dump_config();
}

float LinuxTime::get_setup_priority() const { return setup_priority::HARDWARE; }

}  // namespace linux_time
}  // namespace esphome

#endif  // USE_HOST
