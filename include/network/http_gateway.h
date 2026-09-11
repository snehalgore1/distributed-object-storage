#ifndef DOS_NETWORK_HTTP_GATEWAY_H_
#define DOS_NETWORK_HTTP_GATEWAY_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "cluster/caching_metadata_view.h"
#include "common/metrics.h"
#include "network/coordinator.h"
#include "network/http_server.h"

namespace dos {

// A thin HTTP adapter over the coordinator (spec Milestone 9) with observability
// (spec Milestone 10). It maps HTTP verbs/paths to coordinator calls and Status
// codes to HTTP codes; every request gets a request id (returned as
// X-Request-Id and logged as structured JSON), and metrics are exposed at
// /metrics in Prometheus format.
//
//   PUT    /objects/<key>   body -> Put
//   GET    /objects/<key>        -> Get (bytes)
//   HEAD   /objects/<key>        -> Head (metadata headers only)
//   DELETE /objects/<key>        -> Delete
//   GET    /objects[?prefix=..]  -> List (JSON)
//   GET    /metrics              -> Prometheus exposition
class HttpGateway {
public:
  explicit HttpGateway(std::shared_ptr<Coordinator> coordinator)
      : coordinator_(std::move(coordinator)) {}

  // Optional: lets /metrics report cache hit/miss stats.
  void SetCacheView(std::shared_ptr<CachingMetadataView> cache) { cache_ = std::move(cache); }

  bool Start(const std::string& host, int port);
  int bound_port() const { return server_ ? server_->bound_port() : 0; }
  void Shutdown();

  // Entry point: instruments the request (id, metrics, log) and routes it.
  HttpResponse Handle(const HttpRequest& req);

private:
  HttpResponse Route(const HttpRequest& req);
  std::string MetricsText();

  std::shared_ptr<Coordinator> coordinator_;
  std::shared_ptr<CachingMetadataView> cache_;
  MetricsRegistry metrics_;
  std::atomic<int64_t> in_flight_{0};
  std::unique_ptr<HttpServer> server_;
};

} // namespace dos

#endif // DOS_NETWORK_HTTP_GATEWAY_H_
