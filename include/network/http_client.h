#ifndef DOS_NETWORK_HTTP_CLIENT_H_
#define DOS_NETWORK_HTTP_CLIENT_H_

#include <map>
#include <string>

#include "common/status.h"

namespace dos {

struct HttpClientResponse {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers; // lower-cased header names
};

// Minimal blocking HTTP/1.1 client for the CLI and tests. Sends a single
// request with Content-Length and `Connection: close`, then reads the whole
// response. Not a general-purpose client.
class HttpClient {
public:
  HttpClient(std::string host, int port) : host_(std::move(host)), port_(port) {}

  StatusOr<HttpClientResponse>
  Request(const std::string& method, const std::string& target, const std::string& body = "",
          const std::string& content_type = "application/octet-stream");

private:
  std::string host_;
  int port_;
};

} // namespace dos

#endif // DOS_NETWORK_HTTP_CLIENT_H_
