#ifndef DOS_NETWORK_HTTP_SERVER_H_
#define DOS_NETWORK_HTTP_SERVER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "common/thread_pool.h"

namespace dos {

struct HttpRequest {
  std::string method;
  std::string path;                         // path only (no query)
  std::map<std::string, std::string> query; // parsed query params
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/octet-stream";
  std::string body;
  std::map<std::string, std::string> headers; // extra headers
};

// Minimal blocking HTTP/1.1 server (spec Milestone 9). Deliberately small: it
// parses the request line, headers, and a Content-Length body, dispatches to a
// handler, and writes the response with `Connection: close`. Connections are
// served on a bounded ThreadPool. Not a general-purpose web server — just
// enough to be a thin adapter in front of the coordinator.
class HttpServer {
public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  explicit HttpServer(Handler handler, std::size_t worker_threads = 4);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds `host:port` (port 0 = auto-assign) and starts the accept loop on a
  // background thread. Returns false on bind failure.
  bool Start(const std::string& host, int port);
  int bound_port() const { return bound_port_; }
  void Shutdown();

private:
  void AcceptLoop();
  void ServeConnection(int fd);

  Handler handler_;
  ThreadPool pool_;
  int listen_fd_ = -1;
  int bound_port_ = 0;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
};

} // namespace dos

#endif // DOS_NETWORK_HTTP_SERVER_H_
