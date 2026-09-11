#include "network/http_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dos {
namespace {

bool WriteAll(int fd, const std::string& data) {
  const char* p = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t w = ::write(fd, p, remaining);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += w;
    remaining -= static_cast<std::size_t>(w);
  }
  return true;
}

} // namespace

StatusOr<HttpClientResponse> HttpClient::Request(const std::string& method,
                                                 const std::string& target, const std::string& body,
                                                 const std::string& content_type) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status::IoError("socket: " + std::string(std::strerror(errno)));
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return Status::InvalidArgument("bad host: " + host_);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return Status::Unavailable("connect to " + host_ + " failed: " + std::strerror(errno));
  }

  std::string req = method + " " + target + " HTTP/1.1\r\n";
  req += "Host: " + host_ + "\r\n";
  req += "Content-Type: " + content_type + "\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  if (!WriteAll(fd, req)) {
    ::close(fd);
    return Status::IoError("write request failed");
  }

  std::string resp;
  char chunk[8192];
  while (true) {
    ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0)
      break; // server closed (Connection: close)
    resp.append(chunk, static_cast<std::size_t>(n));
  }
  ::close(fd);

  // Parse "HTTP/1.1 <status> ..." and the body after the header block.
  HttpClientResponse out;
  const auto sp = resp.find(' ');
  if (sp != std::string::npos && resp.size() >= sp + 4) {
    out.status = std::atoi(resp.substr(sp + 1, 3).c_str());
  }
  const auto hdr_end = resp.find("\r\n\r\n");
  if (hdr_end != std::string::npos) {
    out.body = resp.substr(hdr_end + 4);
  }
  return out;
}

} // namespace dos
