#pragma once

#ifdef USE_HOST

#include "esphome/components/canbus/canbus.h"
#include "esphome/core/component.h"

#include <string>

namespace esphome {
namespace socketcan {

// SocketCAN canbus platform: a raw CAN_RAW socket bound to a kernel CAN netdev
// (e.g. can0). No external library -- only <linux/can.h>. The bitrate is NOT
// settable from userspace; configure the interface first with, e.g.,
//   ip link set can0 up type can bitrate 500000
class SocketCAN : public canbus::Canbus {
 public:
  void set_interface(std::string interface) { this->interface_ = std::move(interface); }
  void dump_config() override;

 protected:
  bool setup_internal() override;
  canbus::Error send_message(struct canbus::CanFrame *frame) override;
  canbus::Error read_message(struct canbus::CanFrame *frame) override;

  std::string interface_;
  int socket_fd_{-1};
};

}  // namespace socketcan
}  // namespace esphome

#endif  // USE_HOST
