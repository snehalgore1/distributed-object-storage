#ifndef DOS_NETWORK_HTTP_GATEWAY_H_
#define DOS_NETWORK_HTTP_GATEWAY_H_

#include <memory>
#include <string>

#include "network/coordinator.h"
#include "network/http_server.h"

namespace dos {

// A thin HTTP adapter over the coordinator (spec Milestone 9). It only maps
// HTTP verbs and paths to coordinator calls and Status codes to HTTP codes —
// there is no business logic here.
//
//   PUT    /objects/<key>   body -> Put
//   GET    /objects/<key>        -> Get (bytes)
//   HEAD   /objects/<key>        -> Head (metadata headers only)
//   DELETE /objects/<key>        -> Delete
//   GET    /objects[?prefix=..]  -> List (JSON)
class HttpGateway {
public:
  explicit HttpGateway(std::shared_ptr<Coordinator> coordinator)
      : coordinator_(std::move(coordinator)) {}

  bool Start(const std::string& host, int port);
  int bound_port() const { return server_ ? server_->bound_port() : 0; }
  void Shutdown();

  // Exposed for testing without a socket.
  HttpResponse Handle(const HttpRequest& req);

private:
  std::shared_ptr<Coordinator> coordinator_;
  std::unique_ptr<HttpServer> server_;
};

} // namespace dos

#endif // DOS_NETWORK_HTTP_GATEWAY_H_
