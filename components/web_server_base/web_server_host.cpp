#ifdef USE_HOST

#include "web_server_host.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "esphome/core/log.h"

#ifdef USE_WEBSERVER
#include "esphome/components/web_server/web_server.h"
#endif

namespace esphome {
namespace web_server_idf {

static const char *const TAG = "web_server_host";

// ---------------------------------------------------------------------------
// DefaultHeaders singleton
// ---------------------------------------------------------------------------
static DefaultHeaders &default_headers_instance_() {
  static DefaultHeaders inst;
  return inst;
}
DefaultHeaders &DefaultHeaders::Instance() { return default_headers_instance_(); }

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
static HttpMethod parse_method_(const std::string &m) {
  if (m == "GET") return HTTP_GET;
  if (m == "POST") return HTTP_POST;
  if (m == "PUT") return HTTP_PUT;
  if (m == "DELETE") return HTTP_DELETE;
  if (m == "OPTIONS") return HTTP_OPTIONS;
  if (m == "HEAD") return HTTP_HEAD;
  if (m == "PATCH") return HTTP_PATCH;
  return HTTP_ANY;
}

static std::string url_decode_(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.size()) {
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

static std::string lowercase_(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return s;
}

static bool send_all_(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    data += n;
    len -= n;
  }
  return true;
}

// ---------------------------------------------------------------------------
// AsyncResponseStream::print / printf
// ---------------------------------------------------------------------------
void AsyncResponseStream::print(float value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", value);
  this->content_.append(buf);
}

void AsyncResponseStream::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (needed > 0) {
    size_t old = this->content_.size();
    this->content_.resize(old + needed + 1);
    std::vsnprintf(&this->content_[old], needed + 1, fmt, ap2);
    this->content_.resize(old + needed);
  }
  va_end(ap2);
}

// ---------------------------------------------------------------------------
// AsyncWebServerRequest
// ---------------------------------------------------------------------------
AsyncWebServerRequest::AsyncWebServerRequest(AsyncWebServer *server, int fd, HttpMethod method, std::string url,
                                             std::string query,
                                             std::map<std::string, std::string, std::less<>> headers, std::string body)
    : server_(server),
      fd_(fd),
      method_(method),
      url_(std::move(url)),
      query_(std::move(query)),
      headers_(std::move(headers)),
      body_(std::move(body)) {}

AsyncWebServerRequest::~AsyncWebServerRequest() {
  for (auto *p : this->params_)
    delete p;
  if (this->rsp_ != nullptr)
    delete this->rsp_;
  if (this->fd_ >= 0) {
    ::shutdown(this->fd_, SHUT_RDWR);
    ::close(this->fd_);
    this->fd_ = -1;
  }
}

int AsyncWebServerRequest::take_fd_() {
  int fd = this->fd_;
  this->fd_ = -1;
  return fd;
}

StringRef AsyncWebServerRequest::url_to(std::span<char, URL_BUF_SIZE> buffer) const {
  size_t n = std::min(this->url_.size(), URL_BUF_SIZE - 1);
  std::memcpy(buffer.data(), this->url_.data(), n);
  buffer[n] = '\0';
  return StringRef(buffer.data(), n);
}

optional<std::string> AsyncWebServerRequest::get_header(const char *name) const {
  std::string key = lowercase_(name);
  auto it = this->headers_.find(key);
  if (it == this->headers_.end())
    return {};
  return it->second;
}

void AsyncWebServerRequest::redirect(const std::string &url) {
  auto *r = this->beginResponse(302, "text/html");
  r->addHeader("Location", url.c_str());
  this->send(r);
}

void AsyncWebServerRequest::init_response_(AsyncWebServerResponse *rsp, int code, const char *content_type) {
  rsp->set_code(code);
  rsp->set_content_type(content_type);
  this->rsp_ = rsp;
}

void AsyncWebServerRequest::send(AsyncWebServerResponse *response) {
  this->rsp_ = response;
  this->send_response_();
}

void AsyncWebServerRequest::send(int code, const char *content_type, const char *content) {
  auto *r = new AsyncWebServerResponseContent(this, content == nullptr ? "" : content);
  this->init_response_(r, code, content_type);
  this->send_response_();
}

void AsyncWebServerRequest::send_response_() {
  if (this->response_sent_)
    return;
  this->response_sent_ = true;

  AsyncWebServerResponse *r = this->rsp_;
  int code = r != nullptr ? r->get_code() : 200;
  const std::string &ct = r != nullptr ? r->get_content_type() : *new std::string();
  const char *body = r != nullptr ? r->get_content_data() : nullptr;
  size_t body_len = r != nullptr ? r->get_content_size() : 0;

  const char *reason = "OK";
  if (code == 201) reason = "Created";
  else if (code == 204) reason = "No Content";
  else if (code == 302) reason = "Found";
  else if (code == 400) reason = "Bad Request";
  else if (code == 401) reason = "Unauthorized";
  else if (code == 404) reason = "Not Found";
  else if (code == 405) reason = "Method Not Allowed";
  else if (code == 409) reason = "Conflict";
  else if (code == 500) reason = "Internal Server Error";

  std::string head;
  head.reserve(256);
  char line[160];
  std::snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", code, reason);
  head += line;
  if (!ct.empty()) {
    head += "Content-Type: ";
    head += ct;
    head += "\r\n";
  }
  for (const auto &h : DefaultHeaders::Instance().headers()) {
    head += h.name;
    head += ": ";
    head += h.value;
    head += "\r\n";
  }
  if (r != nullptr) {
    for (const auto &kv : r->get_extra_headers()) {
      head += kv.first;
      head += ": ";
      head += kv.second;
      head += "\r\n";
    }
  }
  std::snprintf(line, sizeof(line), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body_len);
  head += line;

  send_all_(this->fd_, head.data(), head.size());
  if (body != nullptr && body_len > 0)
    send_all_(this->fd_, body, body_len);
}

optional<std::string> AsyncWebServerRequest::find_query_value_(const char *name) const {
  // Search both URL query string and POST body (application/x-www-form-urlencoded).
  auto scan = [&](const std::string &src) -> optional<std::string> {
    size_t pos = 0;
    std::string key(name);
    while (pos < src.size()) {
      size_t amp = src.find('&', pos);
      if (amp == std::string::npos)
        amp = src.size();
      size_t eq = src.find('=', pos);
      if (eq != std::string::npos && eq < amp) {
        if (src.compare(pos, eq - pos, key) == 0)
          return url_decode_(src.substr(eq + 1, amp - eq - 1));
      } else {
        if (src.compare(pos, amp - pos, key) == 0)
          return std::string{};
      }
      pos = amp + 1;
    }
    return {};
  };
  auto v = scan(this->query_);
  if (v.has_value())
    return v;
  auto ct = this->get_header("content-type");
  if (ct.has_value() && ct->find("application/x-www-form-urlencoded") != std::string::npos)
    return scan(this->body_);
  return {};
}

AsyncWebParameter *AsyncWebServerRequest::getParam(const char *name) {
  for (auto *p : this->params_) {
    if (p->name() == name)
      return p;
  }
  auto v = this->find_query_value_(name);
  if (!v.has_value())
    return nullptr;
  auto *p = new AsyncWebParameter(name, *v);
  this->params_.push_back(p);
  return p;
}

bool AsyncWebServerRequest::hasArg(const char *name) { return this->find_query_value_(name).has_value(); }

std::string AsyncWebServerRequest::arg(const char *name) {
  auto v = this->find_query_value_(name);
  return v.value_or("");
}

#ifdef USE_WEBSERVER_AUTH
bool AsyncWebServerRequest::authenticate(const char *, const char *) const { return true; }
void AsyncWebServerRequest::requestAuthentication(const char *) const {}
#endif

// ---------------------------------------------------------------------------
// AsyncWebServer
// ---------------------------------------------------------------------------
void AsyncWebServer::begin() {
  if (this->listen_fd_ >= 0)
    return;
  this->listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->listen_fd_ < 0) {
    ESP_LOGE(TAG, "socket(): %s", strerror(errno));
    return;
  }
  int one = 1;
  ::setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);
  if (::bind(this->listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind(%u): %s", this->port_, strerror(errno));
    ::close(this->listen_fd_);
    this->listen_fd_ = -1;
    return;
  }
  if (::listen(this->listen_fd_, 16) < 0) {
    ESP_LOGE(TAG, "listen(): %s", strerror(errno));
    ::close(this->listen_fd_);
    this->listen_fd_ = -1;
    return;
  }
  this->running_ = true;
  this->accept_thread_ = std::thread(&AsyncWebServer::accept_loop_, this);
  ESP_LOGI(TAG, "Listening on http://0.0.0.0:%u/", this->port_);
}

void AsyncWebServer::end() {
  this->running_ = false;
  if (this->listen_fd_ >= 0) {
    ::shutdown(this->listen_fd_, SHUT_RDWR);
    ::close(this->listen_fd_);
    this->listen_fd_ = -1;
  }
  if (this->accept_thread_.joinable())
    this->accept_thread_.join();
}

void AsyncWebServer::accept_loop_() {
  while (this->running_) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    int fd = ::accept(this->listen_fd_, reinterpret_cast<sockaddr *>(&peer), &plen);
    if (fd < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      if (!this->running_)
        return;
      ESP_LOGW(TAG, "accept(): %s", strerror(errno));
      continue;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::thread(&AsyncWebServer::handle_connection_, this, fd).detach();
  }
}

bool AsyncWebServer::parse_request_(int fd, HttpMethod &method, std::string &url, std::string &query,
                                    std::map<std::string, std::string, std::less<>> &headers, std::string &body) {
  // Read until headers end.
  std::string buf;
  buf.reserve(2048);
  char chunk[1024];
  size_t header_end_pos = std::string::npos;
  while (header_end_pos == std::string::npos) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0)
      return false;
    buf.append(chunk, chunk + n);
    header_end_pos = buf.find("\r\n\r\n");
    if (buf.size() > 32 * 1024)
      return false;
  }

  std::string header_block = buf.substr(0, header_end_pos);
  std::string body_already = buf.substr(header_end_pos + 4);

  // Request line.
  size_t eol = header_block.find("\r\n");
  if (eol == std::string::npos)
    return false;
  std::string req_line = header_block.substr(0, eol);
  size_t sp1 = req_line.find(' ');
  size_t sp2 = req_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos)
    return false;
  method = parse_method_(req_line.substr(0, sp1));
  std::string path_full = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = path_full.find('?');
  if (q == std::string::npos) {
    url = path_full;
    query.clear();
  } else {
    url = path_full.substr(0, q);
    query = path_full.substr(q + 1);
  }
  // web_server.cpp's match_url compares raw URL bytes against decoded entity
  // names ("Relay 1"), so callers using the canonical name form arrive as
  // "/switch/Relay%201/turn_on" and miss every handler. Decode the path here;
  // query stays encoded (find_query_value_ decodes per-parameter).
  url = url_decode_(url);

  // Headers.
  size_t pos = eol + 2;
  while (pos < header_block.size()) {
    size_t e = header_block.find("\r\n", pos);
    if (e == std::string::npos)
      e = header_block.size();
    std::string line = header_block.substr(pos, e - pos);
    pos = e + 2;
    if (line.empty())
      break;
    size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string name = lowercase_(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.erase(value.begin());
    headers[name] = value;
  }

  // Content-Length body slurp (best-effort).
  size_t want = 0;
  auto cl = headers.find("content-length");
  if (cl != headers.end()) {
    char *endptr = nullptr;
    errno = 0;
    unsigned long parsed = std::strtoul(cl->second.c_str(), &endptr, 10);
    if (errno == 0 && endptr != cl->second.c_str())
      want = static_cast<size_t>(parsed);
  }
  body = std::move(body_already);
  while (body.size() < want) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0)
      break;
    body.append(chunk, chunk + n);
  }

  return true;
}

void AsyncWebServer::handle_connection_(int fd) {
  HttpMethod method = HTTP_GET;
  std::string url, query, body;
  std::map<std::string, std::string, std::less<>> headers;
  if (!this->parse_request_(fd, method, url, query, headers, body)) {
    send_simple_(fd, 400, "Bad Request", "bad request\n");
    ::close(fd);
    return;
  }
  auto *req = new AsyncWebServerRequest(this, fd, method, std::move(url), std::move(query), std::move(headers),
                                        std::move(body));
  this->dispatch_(req);
  // If the request's fd was taken (SSE handoff), don't delete the request — the
  // handler now owns it. Otherwise destruct, which closes the socket.
  if (req->get_fd() < 0) {
    // fd handed off; leave request alive (the new owner is responsible).
    return;
  }
  if (!req->response_sent_) {
    // Handler didn't respond. Send a 204.
    req->send(204);
  }
  delete req;
}

void AsyncWebServer::dispatch_(AsyncWebServerRequest *req) {
  std::vector<AsyncWebHandler *> snapshot;
  {
    std::lock_guard<std::mutex> g(this->handlers_mu_);
    snapshot = this->handlers_;
  }
  for (auto *h : snapshot) {
    if (h->canHandle(req)) {
      // Deliver body via handleBody if non-empty (web_server's POST path
      // doesn't actually require it, but match the upstream interface).
      if (!req->body().empty()) {
        h->handleBody(req, reinterpret_cast<uint8_t *>(const_cast<char *>(req->body().data())), req->body().size(), 0,
                      req->body().size());
      }
      h->handleRequest(req);
      return;
    }
  }
  if (this->on_not_found_) {
    this->on_not_found_(req);
    return;
  }
  req->send(404, "text/plain", "not found\n");
}

void AsyncWebServer::send_simple_(int fd, int code, const char *reason, const char *body) {
  char head[256];
  size_t blen = std::strlen(body);
  std::snprintf(head, sizeof(head),
                "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                code, reason, blen);
  send_all_(fd, head, std::strlen(head));
  send_all_(fd, body, blen);
}

// ---------------------------------------------------------------------------
// AsyncEventSource (SSE)
// ---------------------------------------------------------------------------
#ifdef USE_WEBSERVER

AsyncEventSource::~AsyncEventSource() {
  std::lock_guard<std::mutex> g(this->sessions_mu_);
  for (auto *s : this->sessions_)
    delete s;
  this->sessions_.clear();
}

void AsyncEventSource::handleRequest(AsyncWebServerRequest *request) {
  // Take ownership of the socket fd and create a long-lived SSE session.
  // The session lives in this->sessions_ and is serviced by loop() each tick.
  auto *session = new AsyncEventSourceResponse(request, this, this->web_server_);
  {
    std::lock_guard<std::mutex> g(this->sessions_mu_);
    this->sessions_.push_back(session);
  }
  if (this->on_connect_)
    this->on_connect_(session);
  // WebServer::loop() disables itself when no SSE clients are connected. Wake
  // it back up now so subsequent ticks process this session.
  this->web_server_->enable_loop_soon_any_context();
}

bool AsyncEventSource::loop() {
  std::vector<AsyncEventSourceResponse *> snapshot;
  {
    std::lock_guard<std::mutex> g(this->sessions_mu_);
    snapshot = this->sessions_;
  }
  for (auto *s : snapshot)
    s->loop();
  // GC dead sessions.
  std::vector<AsyncEventSourceResponse *> dead;
  {
    std::lock_guard<std::mutex> g(this->sessions_mu_);
    auto it = this->sessions_.begin();
    while (it != this->sessions_.end()) {
      if ((*it)->fd_ < 0) {
        dead.push_back(*it);
        it = this->sessions_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto *d : dead) {
    if (this->on_disconnect_)
      this->on_disconnect_(d);
    delete d;
  }
  return !this->sessions_.empty();
}

void AsyncEventSource::try_send_nodefer(const char *message, const char *event, uint32_t id, uint32_t reconnect) {
  std::lock_guard<std::mutex> g(this->sessions_mu_);
  for (auto *s : this->sessions_)
    s->try_send_nodefer(message, event, id, reconnect);
}

void AsyncEventSource::deferrable_send_state(void *source, const char *event_type,
                                             message_generator_t *message_generator) {
  std::lock_guard<std::mutex> g(this->sessions_mu_);
  for (auto *s : this->sessions_)
    s->deferrable_send_state(source, event_type, message_generator);
}

AsyncEventSourceResponse::AsyncEventSourceResponse(AsyncWebServerRequest *request, AsyncEventSource *server,
                                                   esphome::web_server::WebServer *ws)
    : server_(server), web_server_(ws), entities_iterator_(ws, server) {
  this->fd_ = request->take_fd_();
  // Write SSE headers.
  const char *hdr =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "X-Accel-Buffering: no\r\n"
      "\r\n";
  if (!send_all_(this->fd_, hdr, std::strlen(hdr))) {
    ::close(this->fd_);
    this->fd_ = -1;
    return;
  }
  // Seed the entities iterator so initial state gets dumped as we loop().
  this->entities_iterator_.begin(true);
}

bool AsyncEventSourceResponse::send_raw_(const std::string &chunk) {
  std::lock_guard<std::mutex> g(this->fd_mu_);
  if (this->fd_ < 0)
    return false;
  if (!send_all_(this->fd_, chunk.data(), chunk.size())) {
    ::close(this->fd_);
    this->fd_ = -1;
    return false;
  }
  return true;
}

static std::string build_sse_frame_(const char *message, const char *event, uint32_t id, uint32_t reconnect) {
  std::string frame;
  if (event != nullptr && *event != '\0') {
    frame += "event: ";
    frame += event;
    frame += "\n";
  }
  if (id != 0) {
    char b[32];
    std::snprintf(b, sizeof(b), "id: %u\n", id);
    frame += b;
  }
  if (reconnect != 0) {
    char b[32];
    std::snprintf(b, sizeof(b), "retry: %u\n", reconnect);
    frame += b;
  }
  // Message body — escape any embedded newlines into "data: <chunk>\n" per line.
  const char *p = message;
  while (true) {
    const char *nl = std::strchr(p, '\n');
    frame += "data: ";
    if (nl == nullptr) {
      frame += p;
      frame += "\n";
      break;
    }
    frame.append(p, static_cast<size_t>(nl - p));
    frame += "\n";
    p = nl + 1;
  }
  frame += "\n";
  return frame;
}

bool AsyncEventSourceResponse::try_send_nodefer(const char *message, const char *event, uint32_t id,
                                                uint32_t reconnect) {
  return this->send_raw_(build_sse_frame_(message, event, id, reconnect));
}

void AsyncEventSourceResponse::deq_push_back_with_dedup_(void *source, message_generator_t *message_generator) {
  DeferredEvent ev{source, message_generator};
  for (auto &existing : this->deferred_queue_) {
    if (existing == ev)
      return;
  }
  this->deferred_queue_.push_back(ev);
}

void AsyncEventSourceResponse::deferrable_send_state(void *source, const char *event_type,
                                                     message_generator_t *message_generator) {
  // Match the IDF impl: enqueue. process_deferred_queue_() drains in loop().
  this->deq_push_back_with_dedup_(source, message_generator);
  // event_type ignored on host: WebServer's generators already encode it.
  (void) event_type;
}

void AsyncEventSourceResponse::process_deferred_queue_() {
  while (!this->deferred_queue_.empty()) {
    if (this->fd_ < 0)
      return;
    auto ev = this->deferred_queue_.front();
    auto buf = ev.message_generator_(this->web_server_, ev.source_);
    if (buf.size() > 0) {
      auto frame = build_sse_frame_(buf.c_str(), "state", 0, 0);
      if (!this->send_raw_(frame))
        return;
    }
    this->deferred_queue_.erase(this->deferred_queue_.begin());
  }
}

void AsyncEventSourceResponse::process_buffer_() {
  // Iterate entities; for each, generate a "state" event and push to queue.
  // Loop is bounded per tick to avoid starving the event loop.
  for (int i = 0; i < 16; i++) {
    if (this->entities_iterator_.completed())
      break;
    this->entities_iterator_.advance();
  }
}

void AsyncEventSourceResponse::loop() {
  if (this->fd_ < 0)
    return;
  this->process_buffer_();
  this->process_deferred_queue_();
}

#endif  // USE_WEBSERVER

}  // namespace web_server_idf
}  // namespace esphome

#endif  // USE_HOST
