#pragma once

#ifdef USE_HOST

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef USE_WEBSERVER
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server/list_entities.h"
#endif

namespace esphome {
#ifdef USE_WEBSERVER
namespace web_server {
class WebServer;
}  // namespace web_server
#endif
namespace web_server_idf {

// HTTP method enum compatible with the http_method values used by upstream
// web_server.cpp. Numeric values match libhttp-parser's http_method which is
// what esp_http_server exposes.
enum HttpMethod {
  HTTP_GET = 1,
  HTTP_POST = 3,
  HTTP_PUT = 4,
  HTTP_DELETE = 0,
  HTTP_OPTIONS = 6,
  HTTP_HEAD = 2,
  HTTP_PATCH = 28,
  HTTP_ANY = -1,
};

class AsyncWebServer;
class AsyncWebHandler;
class AsyncWebServerRequest;
class AsyncWebServerResponse;
class AsyncResponseStream;

class AsyncWebParameter {
 public:
  AsyncWebParameter(std::string name, std::string value) : name_(std::move(name)), value_(std::move(value)) {}
  const std::string &name() const { return this->name_; }
  const std::string &value() const { return this->value_; }

 protected:
  std::string name_;
  std::string value_;
};

class AsyncWebServerResponse {
 public:
  AsyncWebServerResponse(const AsyncWebServerRequest *req) : req_(req) {}
  virtual ~AsyncWebServerResponse() = default;

  // NOLINTNEXTLINE(readability-identifier-naming)
  void addHeader(const char *name, const char *value) {
    this->extra_headers_.emplace_back(name, value);
  }

  virtual const char *get_content_data() const = 0;
  virtual size_t get_content_size() const = 0;

  int get_code() const { return this->code_; }
  const std::string &get_content_type() const { return this->content_type_; }
  const std::vector<std::pair<std::string, std::string>> &get_extra_headers() const { return this->extra_headers_; }

  void set_code(int code) { this->code_ = code; }
  void set_content_type(const char *ct) { this->content_type_ = (ct == nullptr ? "" : ct); }

 protected:
  const AsyncWebServerRequest *req_;
  int code_{200};
  std::string content_type_{};
  std::vector<std::pair<std::string, std::string>> extra_headers_{};
};

class AsyncWebServerResponseEmpty : public AsyncWebServerResponse {
 public:
  using AsyncWebServerResponse::AsyncWebServerResponse;
  const char *get_content_data() const override { return nullptr; }
  size_t get_content_size() const override { return 0; }
};

class AsyncWebServerResponseContent : public AsyncWebServerResponse {
 public:
  AsyncWebServerResponseContent(const AsyncWebServerRequest *req, std::string content)
      : AsyncWebServerResponse(req), content_(std::move(content)) {}
  const char *get_content_data() const override { return this->content_.c_str(); }
  size_t get_content_size() const override { return this->content_.size(); }

 protected:
  std::string content_;
};

class AsyncResponseStream : public AsyncWebServerResponse {
 public:
  using AsyncWebServerResponse::AsyncWebServerResponse;
  const char *get_content_data() const override { return this->content_.c_str(); }
  size_t get_content_size() const override { return this->content_.size(); }

  void print(const char *str) { this->content_.append(str); }
  void print(const std::string &str) { this->content_.append(str); }
  void print(float value);
  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void write(uint8_t c) { this->content_.push_back(static_cast<char>(c)); }

 protected:
  std::string content_;
};

class AsyncWebServerResponseProgmem : public AsyncWebServerResponse {
 public:
  AsyncWebServerResponseProgmem(const AsyncWebServerRequest *req, const uint8_t *data, size_t size)
      : AsyncWebServerResponse(req), data_(data), size_(size) {}
  const char *get_content_data() const override { return reinterpret_cast<const char *>(this->data_); }
  size_t get_content_size() const override { return this->size_; }

 protected:
  const uint8_t *data_;
  size_t size_;
};

class AsyncWebServerRequest {
  friend class AsyncWebServer;
#ifdef USE_WEBSERVER
  friend class AsyncEventSource;
  friend class AsyncEventSourceResponse;
#endif

 public:
  ~AsyncWebServerRequest();

  HttpMethod method() const { return this->method_; }
  static constexpr size_t URL_BUF_SIZE = 512;

  // Write URL (without query string) to caller-owned buffer; return StringRef
  // into that buffer.
  StringRef url_to(std::span<char, URL_BUF_SIZE> buffer) const;
  std::string url() const {
    char buf[URL_BUF_SIZE];
    return std::string(this->url_to(buf));
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  size_t contentLength() const { return this->body_.size(); }
  const std::string &body() const { return this->body_; }

#ifdef USE_WEBSERVER_AUTH
  bool authenticate(const char *username, const char *password) const;
  // NOLINTNEXTLINE(readability-identifier-naming)
  void requestAuthentication(const char *realm = nullptr) const;
#endif

  void redirect(const std::string &url);

  // NOLINTNEXTLINE(readability-identifier-naming)
  void send(AsyncWebServerResponse *response);
  // NOLINTNEXTLINE(readability-identifier-naming)
  void send(int code, const char *content_type = nullptr, const char *content = nullptr);

  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebServerResponse *beginResponse(int code, const char *content_type) {
    auto *r = new AsyncWebServerResponseEmpty(this);  // NOLINT(cppcoreguidelines-owning-memory)
    init_response_(r, code, content_type);
    return r;
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebServerResponse *beginResponse(int code, const char *content_type, const std::string &content) {
    auto *r = new AsyncWebServerResponseContent(this, content);  // NOLINT(cppcoreguidelines-owning-memory)
    init_response_(r, code, content_type);
    return r;
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebServerResponse *beginResponse(int code, const char *content_type, const uint8_t *data, size_t size) {
    auto *r = new AsyncWebServerResponseProgmem(this, data, size);  // NOLINT(cppcoreguidelines-owning-memory)
    init_response_(r, code, content_type);
    return r;
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebServerResponse *beginResponse_P(int code, const char *content_type, const uint8_t *data, size_t size) {
    return this->beginResponse(code, content_type, data, size);
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncResponseStream *beginResponseStream(const char *content_type) {
    auto *r = new AsyncResponseStream(this);  // NOLINT(cppcoreguidelines-owning-memory)
    init_response_(r, 200, content_type);
    return r;
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool hasParam(const char *name) { return this->getParam(name) != nullptr; }
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool hasParam(const std::string &name) { return this->getParam(name.c_str()) != nullptr; }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebParameter *getParam(const char *name);
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebParameter *getParam(const std::string &name) { return this->getParam(name.c_str()); }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool hasArg(const char *name);
  std::string arg(const char *name);
  std::string arg(const std::string &name) { return this->arg(name.c_str()); }

  optional<std::string> get_header(const char *name) const;
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool hasHeader(const char *name) const { return this->get_header(name).has_value(); }

  int get_fd() const { return this->fd_; }

 protected:
  AsyncWebServerRequest(AsyncWebServer *server, int fd, HttpMethod method, std::string url, std::string query,
                        std::map<std::string, std::string, std::less<>> headers, std::string body);
  void init_response_(AsyncWebServerResponse *rsp, int code, const char *content_type);
  optional<std::string> find_query_value_(const char *name) const;
  void send_response_();
  // Take ownership of the underlying socket fd. After this call, the request
  // no longer owns it (used to hand off to long-lived SSE sessions).
  int take_fd_();

  AsyncWebServer *server_;
  int fd_{-1};
  HttpMethod method_;
  std::string url_;
  std::string query_;
  std::map<std::string, std::string, std::less<>> headers_;
  std::string body_;
  std::vector<AsyncWebParameter *> params_;
  AsyncWebServerResponse *rsp_{nullptr};
  std::vector<std::pair<std::string, std::string>> response_extra_headers_{};
  int response_code_{200};
  std::string response_content_type_{};
  bool response_sent_{false};
};

class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  // NOLINTNEXTLINE(readability-identifier-naming)
  virtual bool canHandle(AsyncWebServerRequest *request) const { return false; }
  // NOLINTNEXTLINE(readability-identifier-naming)
  virtual void handleRequest(AsyncWebServerRequest *request) {}
  // NOLINTNEXTLINE(readability-identifier-naming)
  virtual void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                            size_t len, bool final) {}
  // NOLINTNEXTLINE(readability-identifier-naming)
  virtual void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {}
  // NOLINTNEXTLINE(readability-identifier-naming)
  virtual bool isRequestHandlerTrivial() const { return true; }
};

class AsyncWebServer {
 public:
  AsyncWebServer(uint16_t port) : port_(port) {}
  ~AsyncWebServer() { this->end(); }

  // NOLINTNEXTLINE(readability-identifier-naming)
  void onNotFound(std::function<void(AsyncWebServerRequest *request)> &&fn) { this->on_not_found_ = std::move(fn); }

  void begin();
  void end();

  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebHandler &addHandler(AsyncWebHandler *handler) {
    std::lock_guard<std::mutex> g(this->handlers_mu_);
    this->handlers_.push_back(handler);
    return *handler;
  }

 protected:
  void accept_loop_();
  void handle_connection_(int client_fd);
  bool parse_request_(int client_fd, HttpMethod &method, std::string &url, std::string &query,
                      std::map<std::string, std::string, std::less<>> &headers, std::string &body);
  void dispatch_(AsyncWebServerRequest *req);
  static void send_simple_(int fd, int code, const char *reason, const char *body);

  uint16_t port_;
  int listen_fd_{-1};
  std::thread accept_thread_;
  std::atomic<bool> running_{false};
  std::vector<AsyncWebHandler *> handlers_;
  std::mutex handlers_mu_;
  std::function<void(AsyncWebServerRequest *)> on_not_found_{};
};

#ifdef USE_WEBSERVER

class AsyncEventSource;
class AsyncEventSourceResponse;

using message_generator_t = json::SerializationBuffer<>(esphome::web_server::WebServer *, void *);

struct DeferredEvent {
  friend class AsyncEventSourceResponse;

 protected:
  void *source_;
  message_generator_t *message_generator_;

 public:
  DeferredEvent(void *source, message_generator_t *message_generator)
      : source_(source), message_generator_(message_generator) {}
  bool operator==(const DeferredEvent &other) const {
    return source_ == other.source_ && message_generator_ == other.message_generator_;
  }
};

class AsyncEventSourceResponse {
  friend class AsyncEventSource;

 public:
  bool try_send_nodefer(const char *message, const char *event = nullptr, uint32_t id = 0, uint32_t reconnect = 0);
  void deferrable_send_state(void *source, const char *event_type, message_generator_t *message_generator);
  void loop();

 protected:
  AsyncEventSourceResponse(AsyncWebServerRequest *request, AsyncEventSource *server,
                           esphome::web_server::WebServer *ws);

  bool send_raw_(const std::string &chunk);
  void deq_push_back_with_dedup_(void *source, message_generator_t *message_generator);
  void process_deferred_queue_();
  void process_buffer_();

  AsyncEventSource *server_;
  int fd_{-1};
  std::mutex fd_mu_;
  std::vector<DeferredEvent> deferred_queue_;
  esphome::web_server::WebServer *web_server_;
  // ListEntitiesIterator's host-branch ctor is (WebServer*, AsyncEventSource*).
  // It walks entity types and queues a deferred event per entity on `server_`.
  esphome::web_server::ListEntitiesIterator entities_iterator_;
  std::string event_buffer_;
  size_t event_bytes_sent_{0};
};

using AsyncEventSourceClient = AsyncEventSourceResponse;

class AsyncEventSource : public AsyncWebHandler {
  friend class AsyncEventSourceResponse;
  using connect_handler_t = std::function<void(AsyncEventSourceClient *)>;

 public:
  AsyncEventSource(std::string url, esphome::web_server::WebServer *ws) : url_(std::move(url)), web_server_(ws) {}
  ~AsyncEventSource() override;

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(buf) == this->url_;
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override;
  // NOLINTNEXTLINE(readability-identifier-naming)
  void onConnect(connect_handler_t &&cb) { this->on_connect_ = std::move(cb); }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void onDisconnect(std::function<void(AsyncEventSourceClient *)> &&cb) { this->on_disconnect_ = std::move(cb); }

  void try_send_nodefer(const char *message, const char *event = nullptr, uint32_t id = 0, uint32_t reconnect = 0);
  void deferrable_send_state(void *source, const char *event_type, message_generator_t *message_generator);
  bool loop();
  bool empty() { return this->count() == 0; }
  size_t count() const { return this->sessions_.size(); }

 protected:
  std::string url_;
  std::vector<AsyncEventSourceResponse *> sessions_;
  std::mutex sessions_mu_;
  connect_handler_t on_connect_{};
  std::function<void(AsyncEventSourceClient *)> on_disconnect_{};
  esphome::web_server::WebServer *web_server_;
};

#endif  // USE_WEBSERVER

struct HttpHeader {
  const char *name;
  const char *value;
};

#ifndef WEB_SERVER_DEFAULT_HEADERS_COUNT
#define WEB_SERVER_DEFAULT_HEADERS_COUNT 1
#endif

class DefaultHeaders {
  friend class AsyncWebServerRequest;
#ifdef USE_WEBSERVER
  friend class AsyncEventSourceResponse;
#endif

 public:
  // NOLINTNEXTLINE(readability-identifier-naming)
  void addHeader(const char *name, const char *value) { this->headers_.push_back({name, value}); }
  // NOLINTNEXTLINE(readability-identifier-naming)
  static DefaultHeaders &Instance();
  const std::vector<HttpHeader> &headers() const { return this->headers_; }

 protected:
  std::vector<HttpHeader> headers_{};
};

}  // namespace web_server_idf
}  // namespace esphome

// Upstream uses unqualified Async* names (via `using namespace
// esphome::web_server_idf;` at the bottom of web_server_idf.h on USE_ESP32).
using namespace esphome::web_server_idf;  // NOLINT(google-global-names-in-headers)

#endif  // USE_HOST
