#ifdef USE_HOST
#if defined(__linux__)

#include "socketcan.h"

#include "esphome/core/log.h"

#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace esphome {
namespace socketcan {

static const char *const TAG = "socketcan";

bool SocketCAN::setup_internal() {
  this->socket_fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (this->socket_fd_ < 0) {
    if (errno == EPERM || errno == EACCES) {
      ESP_LOGE(TAG, "socket(PF_CAN) permission denied. Grant with: sudo setcap 'cap_net_raw=eip' <binary>");
    } else {
      ESP_LOGE(TAG, "socket(PF_CAN) failed: %s", strerror(errno));
    }
    this->mark_failed();
    return false;
  }

  // Resolve interface name -> index.
  struct ifreq ifr{};
  strncpy(ifr.ifr_name, this->interface_.c_str(), sizeof(ifr.ifr_name) - 1);
  if (ioctl(this->socket_fd_, SIOCGIFINDEX, &ifr) < 0) {
    ESP_LOGE(TAG, "interface '%s' not found: %s. Bring it up first, e.g. ip link set %s up type can bitrate 500000",
             this->interface_.c_str(), strerror(errno), this->interface_.c_str());
    ::close(this->socket_fd_);
    this->socket_fd_ = -1;
    this->mark_failed();
    return false;
  }

  struct sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (::bind(this->socket_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind(%s) failed: %s", this->interface_.c_str(), strerror(errno));
    ::close(this->socket_fd_);
    this->socket_fd_ = -1;
    this->mark_failed();
    return false;
  }

  // Non-blocking so read_message() never stalls loop().
  int flags = fcntl(this->socket_fd_, F_GETFL, 0);
  fcntl(this->socket_fd_, F_SETFL, flags | O_NONBLOCK);

  ESP_LOGCONFIG(TAG, "  Interface: %s (fd=%d)", this->interface_.c_str(), this->socket_fd_);
  return true;
}

void SocketCAN::dump_config() {
  ESP_LOGCONFIG(TAG, "SocketCAN:");
  ESP_LOGCONFIG(TAG, "  Interface: %s", this->interface_.c_str());
  // The CAN bitrate cannot be set from userspace via the socket API; the netdev
  // must be configured externally (ip link set <if> up type can bitrate N).
  ESP_LOGCONFIG(TAG, "  Bit rate: informational only; configure externally via ip-link(8)");
  if (this->socket_fd_ < 0)
    ESP_LOGE(TAG, "  Setup failed!");
}

canbus::Error SocketCAN::send_message(struct canbus::CanFrame *frame) {
  if (this->socket_fd_ < 0)
    return canbus::ERROR_FAILINIT;

  struct can_frame cf{};
  // Mask the id to its valid width before OR-ing flags, so stray high bits
  // can't leak into the EFF/RTR flag region of can_id.
  cf.can_id = frame->use_extended_id ? (frame->can_id & CAN_EFF_MASK) : (frame->can_id & CAN_SFF_MASK);
  if (frame->use_extended_id)
    cf.can_id |= CAN_EFF_FLAG;
  if (frame->remote_transmission_request)
    cf.can_id |= CAN_RTR_FLAG;
  uint8_t len = frame->can_data_length_code > sizeof(cf.data) ? sizeof(cf.data) : frame->can_data_length_code;
  cf.can_dlc = len;
  memcpy(cf.data, frame->data, len);

  ssize_t ret = ::write(this->socket_fd_, &cf, sizeof(cf));
  if (ret < 0) {
    // Transient back-pressure or a down interface -> report busy without
    // logging; loop() retries and a down link would otherwise flood the log.
    if (errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENETDOWN)
      return canbus::ERROR_ALLTXBUSY;
    ESP_LOGW(TAG, "write() failed: %s", strerror(errno));
    return canbus::ERROR_FAILTX;
  }
  return canbus::ERROR_OK;
}

canbus::Error SocketCAN::read_message(struct canbus::CanFrame *frame) {
  if (this->socket_fd_ < 0)
    return canbus::ERROR_FAILINIT;

  struct can_frame cf{};
  ssize_t ret = ::read(this->socket_fd_, &cf, sizeof(cf));
  if (ret < 0) {
    // Nothing ready, interrupted, or interface down -> idle quietly; logging
    // here floods loop() the whole time the link is down.
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENETDOWN)
      return canbus::ERROR_NOMSG;
    ESP_LOGW(TAG, "read() failed: %s", strerror(errno));
    return canbus::ERROR_FAIL;
  }
  if (ret != static_cast<ssize_t>(sizeof(cf)))
    return canbus::ERROR_NOMSG;  // not a classic CAN frame (e.g. CAN FD/XL) -> skip, don't poison the drain
  // Error frames carry diagnostic bits, not a payload the canbus layer can
  // represent; drop rather than decode noise into a normal frame.
  if (cf.can_id & CAN_ERR_FLAG)
    return canbus::ERROR_NOMSG;

  frame->use_extended_id = (cf.can_id & CAN_EFF_FLAG) != 0;
  frame->remote_transmission_request = (cf.can_id & CAN_RTR_FLAG) != 0;
  frame->can_id = cf.can_id & (frame->use_extended_id ? CAN_EFF_MASK : CAN_SFF_MASK);
  uint8_t len = cf.can_dlc > sizeof(cf.data) ? sizeof(cf.data) : cf.can_dlc;
  frame->can_data_length_code = len;
  memcpy(frame->data, cf.data, len);
  return canbus::ERROR_OK;
}

}  // namespace socketcan
}  // namespace esphome

#endif  // defined(__linux__)
#endif  // USE_HOST
