#pragma once
#include "esphome/core/defines.h"
#ifdef USE_NETWORK
#include <utility>
#include <vector>

#include "esphome/core/progmem.h"

#if USE_ESP32
#include "esphome/core/hal.h"
#include "esphome/components/web_server_idf/web_server_idf.h"
#elif defined(USE_HOST)
#include "web_server_host.h"
#else
#include <ESPAsyncWebServer.h>
#endif

#if USE_ESP32 || defined(USE_HOST)
using PlatformString = std::string;
#elif USE_ARDUINO
using PlatformString = String;
#endif

namespace esphome::web_server_base {

class WebServerBase;
extern WebServerBase *global_web_server_base;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

namespace internal {

class MiddlewareHandler : public AsyncWebHandler {
 public:
  MiddlewareHandler(AsyncWebHandler *next) : next_(next) {}

  bool canHandle(AsyncWebServerRequest *request) const override { return next_->canHandle(request); }
  void handleRequest(AsyncWebServerRequest *request) override { next_->handleRequest(request); }
  void handleUpload(AsyncWebServerRequest *request, const PlatformString &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override {
    next_->handleUpload(request, filename, index, data, len, final);
  }
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    next_->handleBody(request, data, len, index, total);
  }
  bool isRequestHandlerTrivial() const override { return next_->isRequestHandlerTrivial(); }

 protected:
  AsyncWebHandler *next_;
};

#ifdef USE_WEBSERVER_AUTH
// All fields point to string literals in generated code; nothing is copied.
// Host supports basic auth only (digest is rejected at config time), so the
// credentials are the base64("username:password") hash web_server precomputes.
struct Credentials {
  const char *basic_auth_hash{nullptr};
  bool is_set() const { return basic_auth_hash != nullptr; }
};

class AuthMiddlewareHandler : public MiddlewareHandler {
 public:
  AuthMiddlewareHandler(AsyncWebHandler *next, Credentials *credentials)
      : MiddlewareHandler(next), credentials_(credentials) {}

  bool check_auth(AsyncWebServerRequest *request) {
    bool success = request->authenticate(credentials_->basic_auth_hash);
    if (!success) {
      request->requestAuthentication();
    }
    return success;
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    if (!check_auth(request))
      return;
    MiddlewareHandler::handleRequest(request);
  }
  void handleUpload(AsyncWebServerRequest *request, const PlatformString &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override {
    if (!check_auth(request))
      return;
    MiddlewareHandler::handleUpload(request, filename, index, data, len, final);
  }
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    if (!check_auth(request))
      return;
    MiddlewareHandler::handleBody(request, data, len, index, total);
  }

 protected:
  Credentials *credentials_;
};
#endif

}  // namespace internal

class WebServerBase final {
 public:
  // Mirrors upstream (2026.9): the server is created once and never deleted;
  // init()/deinit() refcount users and start/stop the listener.
  void init() {
    this->initialized_++;
    if (this->server_ != nullptr) {
      if (this->initialized_ == 1) {
        // Restart the listener after a previous deinit()
        this->server_->begin();
      }
      return;
    }
    this->server_ = new AsyncWebServer(this->port_);
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    this->server_->begin();

    for (auto *handler : this->handlers_)
      this->server_->addHandler(handler);
  }
  void deinit() {
    if (this->initialized_ == 0)
      return;  // unbalanced deinit()
    this->initialized_--;
    if (this->initialized_ == 0) {
      this->server_->end();
    }
  }
  AsyncWebServer *get_server() const { return this->server_; }

#ifdef USE_WEBSERVER_AUTH
  void set_auth_basic_hash(const char *hash) { credentials_.basic_auth_hash = hash; }
#endif

  void add_handler(AsyncWebHandler *handler);
  void add_handler_without_auth(AsyncWebHandler *handler);

  void set_port(uint16_t port) { port_ = port; }
  uint16_t get_port() const { return port_; }

 protected:
  uint8_t initialized_{0};
  uint16_t port_{80};
  AsyncWebServer *server_{nullptr};
  std::vector<AsyncWebHandler *> handlers_;
#ifdef USE_WEBSERVER_AUTH
  internal::Credentials credentials_;
#endif
};

}  // namespace esphome::web_server_base
#endif
