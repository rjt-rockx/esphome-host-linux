#ifdef USE_HOST

#include "ble_host_thread.h"

#include "esphome/core/log.h"

#include <cerrno>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

#include <systemd/sd-event.h>

namespace esphome {
namespace esp32_ble {

static const char *const TAG = "ble_worker";

BleWorker &BleWorker::instance() {
  static BleWorker inst;
  return inst;
}

// eventfd wake handler: just drains the counter; the real work (servicing each
// client's command queue) happens after sd_event_run returns in run_().
static int wake_handler(sd_event_source * /*s*/, int fd, uint32_t /*revents*/, void * /*userdata*/) {
  uint64_t v;
  ssize_t r = read(fd, &v, sizeof(v));
  (void) r;  // best-effort drain
  return 0;
}

void BleWorker::ensure_started_() {
  bool expected = false;
  if (!this->running_.compare_exchange_strong(expected, true))
    return;  // already started

  this->wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "eventfd() failed: %s", std::strerror(errno));
    this->running_ = false;
    return;
  }
  this->thread_ = std::thread([this] { this->run_(); });
  // Process-lifetime daemon worker: detach so the joinable std::thread doesn't
  // std::terminate at exit (the singleton outlives all clients).
  this->thread_.detach();
}

void BleWorker::run_() {
  if (sd_event_new(&this->event_) < 0) {
    ESP_LOGE(TAG, "sd_event_new failed");
    return;
  }
  sd_event_add_io(this->event_, &this->wake_source_, this->wake_fd_, EPOLLIN, wake_handler, this);

  while (!this->stop_.load()) {
    // Service the loop; 1s timeout bounds shutdown latency. Each return is an
    // opportunity to run any newly-queued per-client commands.
    int r = sd_event_run(this->event_, 1000000 /* us */);
    if (r < 0) {
      ESP_LOGW(TAG, "sd_event_run: %s", std::strerror(-r));
      break;
    }
    std::vector<BusWorkerClient *> snapshot;
    {
      std::lock_guard<std::mutex> g(this->clients_mu_);
      snapshot = this->worker_clients_;
    }
    for (auto *c : snapshot)
      c->worker_process_commands();
  }

  if (this->wake_source_ != nullptr)
    sd_event_source_unref(this->wake_source_);
  if (this->event_ != nullptr)
    this->event_ = sd_event_unref(this->event_);
  if (this->wake_fd_ >= 0)
    ::close(this->wake_fd_);
}

void BleWorker::attach_worker_client(BusWorkerClient *client) {
  this->ensure_started_();
  {
    std::lock_guard<std::mutex> g(this->clients_mu_);
    this->worker_clients_.push_back(client);
  }
  this->wake();
}

void BleWorker::detach_worker_client(BusWorkerClient *client) {
  std::lock_guard<std::mutex> g(this->clients_mu_);
  for (auto it = this->worker_clients_.begin(); it != this->worker_clients_.end(); ++it) {
    if (*it == client) {
      this->worker_clients_.erase(it);
      break;
    }
  }
}

void BleWorker::wake() {
  if (this->wake_fd_ < 0)
    return;
  uint64_t v = 1;
  ssize_t r = write(this->wake_fd_, &v, sizeof(v));
  (void) r;
}

}  // namespace esp32_ble
}  // namespace esphome

#endif  // USE_HOST
