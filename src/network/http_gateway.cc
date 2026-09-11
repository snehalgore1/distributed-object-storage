#include "network/http_gateway.h"

#include <chrono>
#include <sstream>

#include "common/logging.h"

namespace dos {
namespace {

int HttpStatusFor(StatusCode code) {
  switch (code) {
  case StatusCode::kOk:
    return 200;
  case StatusCode::kNotFound:
    return 404;
  case StatusCode::kConflict:
    return 409;
  case StatusCode::kInvalidArgument:
    return 400;
  case StatusCode::kChecksumMismatch:
    return 422;
  case StatusCode::kUnavailable:
    return 503;
  case StatusCode::kAlreadyExists:
    return 409;
  case StatusCode::kIoError:
    return 500;
  }
  return 500;
}

HttpResponse TextError(const Status& s) {
  HttpResponse r;
  r.status = HttpStatusFor(s.code());
  r.content_type = "text/plain";
  r.body = s.ToString() + "\n";
  return r;
}

// Minimal JSON string escaping for keys/checksums (which are ASCII-safe here).
std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

} // namespace

HttpResponse HttpGateway::Handle(const HttpRequest& req) {
  // /metrics is not counted as an object operation.
  if (req.path == "/metrics") {
    HttpResponse r;
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    r.body = MetricsText();
    return r;
  }

  const std::string request_id = NewRequestId();
  metrics_.SetGauge("dos_http_in_flight", "requests being served",
                    static_cast<double>(in_flight_.fetch_add(1) + 1));
  const auto t0 = std::chrono::steady_clock::now();

  HttpResponse resp = Route(req);

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  metrics_.SetGauge("dos_http_in_flight", "requests being served",
                    static_cast<double>(in_flight_.fetch_sub(1) - 1));
  metrics_.IncCounter("dos_http_requests_total", "total HTTP requests", 1,
                      {{"method", req.method}, {"code", std::to_string(resp.status)}});
  if (resp.status >= 500) {
    metrics_.IncCounter("dos_http_errors_total", "5xx responses", 1, {{"method", req.method}});
  }
  metrics_.ObserveHistogram("dos_http_request_duration_seconds", "request latency", secs,
                            {{"method", req.method}});
  resp.headers["X-Request-Id"] = request_id;

  LogInfo("http_request", {{"request_id", request_id},
                           {"method", req.method},
                           {"path", req.path},
                           {"status", std::to_string(resp.status)},
                           {"duration_ms", std::to_string(secs * 1000.0)}});
  return resp;
}

std::string HttpGateway::MetricsText() {
  if (cache_) {
    metrics_.SetGauge("dos_cache_hits", "metadata cache hits", static_cast<double>(cache_->hits()));
    metrics_.SetGauge("dos_cache_misses", "metadata cache misses",
                      static_cast<double>(cache_->misses()));
    metrics_.SetGauge("dos_cache_hit_ratio", "metadata cache hit ratio", cache_->hit_rate());
  }
  // Replica health from the coordinator's point of view.
  int healthy = 0, lagging = 0, failed = 0;
  for (const auto& [id, h] : coordinator_->ReplicaHealthSnapshot()) {
    (void)id;
    if (h == ReplicaHealth::kHealthy) {
      ++healthy;
    } else if (h == ReplicaHealth::kLagging) {
      ++lagging;
    } else {
      ++failed;
    }
  }
  metrics_.SetGauge("dos_replicas", "replica count by state", healthy, {{"state", "healthy"}});
  metrics_.SetGauge("dos_replicas", "replica count by state", lagging, {{"state", "lagging"}});
  metrics_.SetGauge("dos_replicas", "replica count by state", failed, {{"state", "failed"}});
  return metrics_.Serialize();
}

HttpResponse HttpGateway::Route(const HttpRequest& req) {
  static const std::string kPrefix = "/objects";
  if (req.path.rfind(kPrefix, 0) != 0) {
    HttpResponse r;
    r.status = 404;
    r.content_type = "text/plain";
    r.body = "not found; use /objects/<key>\n";
    return r;
  }

  // key = path after "/objects/"; empty means the collection (list).
  std::string key;
  if (req.path.size() > kPrefix.size() + 1) {
    key = req.path.substr(kPrefix.size() + 1); // skip "/objects/"
  }

  if (req.method == "PUT") {
    if (key.empty())
      return TextError(Status::InvalidArgument("PUT requires a key"));
    auto r = coordinator_->Put(key, req.body);
    if (!r.ok())
      return TextError(r.status());
    HttpResponse resp;
    resp.status = 201;
    resp.content_type = "text/plain";
    resp.headers["X-Object-Version"] = std::to_string(r.value().version);
    resp.body = "stored " + key + " (v" + std::to_string(r.value().version) + ")\n";
    return resp;
  }

  if (req.method == "GET") {
    if (key.empty()) {
      std::string prefix;
      auto it = req.query.find("prefix");
      if (it != req.query.end())
        prefix = it->second;
      auto list = coordinator_->List(prefix);
      if (!list.ok())
        return TextError(list.status());
      std::ostringstream json;
      json << "[";
      for (std::size_t i = 0; i < list.value().size(); ++i) {
        const auto& m = list.value()[i];
        if (i)
          json << ",";
        json << "{\"key\":\"" << JsonEscape(m.key) << "\",\"version\":" << m.version
             << ",\"size\":" << m.size << ",\"checksum\":\"" << JsonEscape(m.checksum) << "\"}";
      }
      json << "]";
      HttpResponse resp;
      resp.status = 200;
      resp.content_type = "application/json";
      resp.body = json.str();
      return resp;
    }
    auto r = coordinator_->Get(key);
    if (!r.ok())
      return TextError(r.status());
    HttpResponse resp;
    resp.status = 200;
    resp.body = std::move(r).value();
    return resp;
  }

  if (req.method == "HEAD") {
    if (key.empty())
      return TextError(Status::InvalidArgument("HEAD requires a key"));
    auto r = coordinator_->Head(key);
    HttpResponse resp;
    if (!r.ok()) {
      resp.status = HttpStatusFor(r.status().code());
      return resp; // HEAD carries no body
    }
    resp.status = 200;
    resp.headers["X-Object-Version"] = std::to_string(r.value().version);
    resp.headers["X-Object-Size"] = std::to_string(r.value().size);
    resp.headers["X-Object-Checksum"] = r.value().checksum;
    return resp;
  }

  if (req.method == "DELETE") {
    if (key.empty())
      return TextError(Status::InvalidArgument("DELETE requires a key"));
    Status s = coordinator_->Delete(key);
    if (!s.ok())
      return TextError(s);
    HttpResponse resp;
    resp.status = 204;
    return resp;
  }

  HttpResponse resp;
  resp.status = 400;
  resp.content_type = "text/plain";
  resp.body = "unsupported method: " + req.method + "\n";
  return resp;
}

bool HttpGateway::Start(const std::string& host, int port) {
  server_ = std::make_unique<HttpServer>([this](const HttpRequest& req) { return Handle(req); });
  return server_->Start(host, port);
}

void HttpGateway::Shutdown() {
  if (server_)
    server_->Shutdown();
}

} // namespace dos
