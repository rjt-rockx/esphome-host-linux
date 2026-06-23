#ifdef USE_HOST

#include "linux_spi.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace linux_spi {

static const char *const TAG = "linux_spi";

// spidev bounds a single SPI_IOC_MESSAGE by its module 'bufsiz' parameter
// (default 4096); larger transfers are split into chunks of this size.
static const size_t SPI_CHUNK_MAX = 4096;

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

// Program this device's mode on the shared fd once per transaction. The
// spi_ioc_transfer struct carries speed/bits but not CPOL/CPHA, so mode is set
// via ioctl here rather than per byte. The return must be checked: a controller
// without hardware LSB-first rejects that bit, and silently running MSB-first
// would corrupt data -- so drop the bit, warn once, and continue (subsequent
// transactions then succeed because kernel_mode_ no longer requests it).
void LinuxSPIDelegate::begin_transaction() {
  if (this->fd_ >= 0 && ioctl(this->fd_, SPI_IOC_WR_MODE, &this->kernel_mode_) < 0) {
    if (this->kernel_mode_ & SPI_LSB_FIRST) {
      ESP_LOGW(TAG, "Controller rejected LSB-first; falling back to MSB-first (data may need byte-reversal)");
      this->kernel_mode_ &= ~SPI_LSB_FIRST;
      if (ioctl(this->fd_, SPI_IOC_WR_MODE, &this->kernel_mode_) < 0)
        ESP_LOGW(TAG, "SPI_IOC_WR_MODE failed: %s", strerror(errno));
    } else {
      ESP_LOGW(TAG, "SPI_IOC_WR_MODE failed: %s", strerror(errno));
    }
  }
  spi::SPIDelegate::begin_transaction();  // base toggles cs_pin_ (no-op for NULL_PIN)
}

void LinuxSPIDelegate::do_transfer_(const uint8_t *tx, uint8_t *rx, size_t length) {
  if (this->fd_ < 0 || length == 0)
    return;

  for (size_t off = 0; off < length; off += SPI_CHUNK_MAX) {
    size_t chunk = std::min(SPI_CHUNK_MAX, length - off);
    struct spi_ioc_transfer xfer{};
    xfer.tx_buf = tx ? reinterpret_cast<__u64>(tx + off) : 0;
    xfer.rx_buf = rx ? reinterpret_cast<__u64>(rx + off) : 0;
    xfer.len = static_cast<__u32>(chunk);
    xfer.speed_hz = this->data_rate_;
    xfer.bits_per_word = this->bits_per_word_;
    xfer.cs_change = 0;

    int rc = ioctl(this->fd_, SPI_IOC_MESSAGE(1), &xfer);
    if (rc < static_cast<int>(chunk)) {
      ESP_LOGW(TAG, "SPI_IOC_MESSAGE failed (%d/%zu): %s", rc, chunk, strerror(errno));
      return;
    }
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
    int err = errno;
    if (err == ENOENT) {
      ESP_LOGE(TAG, "[%s] device not found (enable the SPI overlay, e.g. dtparam=spi=on)",
               this->device_path_.c_str());
    } else if (err == EACCES) {
      ESP_LOGE(TAG, "[%s] permission denied (add user to 'spi' group)", this->device_path_.c_str());
    } else {
      ESP_LOGE(TAG, "[%s] failed to open: %s", this->device_path_.c_str(), strerror(err));
    }
    this->mark_failed();
    return;
  }

  uint8_t bits = 8;
  if (ioctl(this->fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
    ESP_LOGW(TAG, "SPI_IOC_WR_BITS_PER_WORD failed: %s", strerror(errno));

  // Drive the spidev fd directly; no GPIO pin setup needed.
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
