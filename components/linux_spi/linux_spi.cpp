#ifdef USE_HOST

#include "linux_spi.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace linux_spi {

static const char *const TAG = "linux_spi";

// Map ESPHome's mode enum to the kernel's CPOL/CPHA bit pattern.
static uint8_t kernel_mode_from(spi::SPIMode mode) {
  switch (mode) {
    case spi::MODE0:
      return 0;
    case spi::MODE1:
      return SPI_CPHA;
    case spi::MODE2:
      return SPI_CPOL;
    case spi::MODE3:
      return SPI_CPOL | SPI_CPHA;
    default:
      return 0;
  }
}

LinuxSPIDelegate::LinuxSPIDelegate(int fd, uint32_t data_rate, spi::SPIBitOrder bit_order, spi::SPIMode mode,
                                   GPIOPin *cs_pin)
    : spi::SPIDelegate(data_rate, bit_order, mode, cs_pin), fd_(fd) {
  this->kernel_mode_ = kernel_mode_from(mode);
  if (bit_order == spi::BIT_ORDER_LSB_FIRST)
    this->kernel_mode_ |= SPI_LSB_FIRST;
}

void LinuxSPIDelegate::do_transfer_(const uint8_t *tx, uint8_t *rx, size_t length) {
  if (this->fd_ < 0 || length == 0)
    return;

  struct spi_ioc_transfer xfer{};
  xfer.tx_buf = reinterpret_cast<__u64>(tx);
  xfer.rx_buf = reinterpret_cast<__u64>(rx);
  xfer.len = static_cast<__u32>(length);
  xfer.speed_hz = this->data_rate_;
  xfer.bits_per_word = this->bits_per_word_;
  xfer.cs_change = 0;
  // SPI mode is configured at the fd level via SPI_IOC_WR_MODE before each
  // transaction; cheaper than per-message reconfiguration since the kernel
  // caches state and only reprograms the controller on change.
  ioctl(this->fd_, SPI_IOC_WR_MODE, &this->kernel_mode_);

  int rc = ioctl(this->fd_, SPI_IOC_MESSAGE(1), &xfer);
  if (rc < 0) {
    ESP_LOGW(TAG, "SPI_IOC_MESSAGE failed: %s", strerror(errno));
  }
}

uint8_t LinuxSPIDelegate::transfer(uint8_t data) {
  uint8_t rx = 0;
  this->do_transfer_(&data, &rx, 1);
  return rx;
}

void LinuxSPIDelegate::transfer(uint8_t *ptr, size_t length) { this->do_transfer_(ptr, ptr, length); }

void LinuxSPIDelegate::transfer(const uint8_t *txbuf, uint8_t *rxbuf, size_t length) {
  this->do_transfer_(txbuf, rxbuf, length);
}

void LinuxSPIDelegate::write_array(const uint8_t *ptr, size_t length) { this->do_transfer_(ptr, nullptr, length); }

void LinuxSPIDelegate::read_array(uint8_t *ptr, size_t length) {
  std::memset(ptr, 0, length);
  this->do_transfer_(ptr, ptr, length);
}

void LinuxSPIDelegate::write16(uint16_t data) {
  uint8_t buf[2];
  if (this->bit_order_ == spi::BIT_ORDER_MSB_FIRST) {
    buf[0] = (data >> 8) & 0xFF;
    buf[1] = data & 0xFF;
  } else {
    buf[0] = data & 0xFF;
    buf[1] = (data >> 8) & 0xFF;
  }
  this->do_transfer_(buf, nullptr, 2);
}

void LinuxSPIComponent::setup() {
  this->fd_ = open(this->device_path_.c_str(), O_RDWR);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "Failed to open %s: %s", this->device_path_.c_str(), strerror(errno));
    this->mark_failed();
    return;
  }

  uint8_t bits = 8;
  if (ioctl(this->fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
    ESP_LOGW(TAG, "SPI_IOC_WR_BITS_PER_WORD failed: %s", strerror(errno));

  // Skip the upstream pin setup; install our LinuxSPIBus directly.
  this->spi_bus_ = new LinuxSPIBus(this->fd_);  // NOLINT(cppcoreguidelines-owning-memory)
  ESP_LOGI(TAG, "Opened %s (fd=%d)", this->device_path_.c_str(), this->fd_);
}

void LinuxSPIComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SPI (Linux spidev) bus:");
  ESP_LOGCONFIG(TAG, "  Device: %s", this->device_path_.c_str());
  ESP_LOGCONFIG(TAG, "  fd: %d", this->fd_);
}

}  // namespace linux_spi
}  // namespace esphome

#endif  // USE_HOST
