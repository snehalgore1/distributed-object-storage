#include "network/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace dos {
namespace {

const char* ReasonPhrase(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 409:
    return "Conflict";
  case 422:
    return "Unprocessable Entity";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "OK";
  }
}

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

// Splits "/objects/foo?prefix=bar" into path and query params.
void ParseTarget(const std::string& target, HttpRequest* req) {
  const auto q = target.find('?');
  req->path = target.substr(0, q);
  if (q == std::string::npos)
    return;
  std::string query = target.substr(q + 1);
  std::stringstream ss(query);
  std::string pair;
  while (std::getline(ss, pair, '&')) {
    const auto eq = pair.find('=');
    if (eq == std::string::npos) {
      req->query[pair] = "";
    } else {
      req->query[pair.substr(0, eq)] = pair.substr(eq + 1);
    }
  }
}

} // namespace

HttpServer::HttpServer(Handler handler, std::size_t worker_threads)
    : handler_(std::move(handler)), pool_(worker_threads, 1024) {}

HttpServer::~HttpServer() { Shutdown(); }

bool HttpServer::Start(const std::string& host, int port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0)
    return false;
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 64) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    bound_port_ = ntohs(addr.sin_port);
  }
  running_.store(true);
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return true;
}

void HttpServer::AcceptLoop() {
  while (running_.load()) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_.load())
        break;
      if (errno == EINTR)
        continue;
      break;
    }
    // Serve on the pool; if saturated, handle inline rather than drop.
    if (!pool_.Submit([this, fd] { ServeConnection(fd); }).ok()) {
      ServeConnection(fd);
    }
  }
}

void HttpServer::ServeConnection(int fd) {
  std::string buf;
  char chunk[8192];

  // Read until we have the full header block.
  std::size_t header_end = std::string::npos;
  while (true) {
    header_end = buf.find("\r\n\r\n");
    if (header_end != std::string::npos)
      break;
    ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      ::close(fd);
      return;
    }
    buf.append(chunk, static_cast<std::size_t>(n));
  }

  // Parse request line + headers.
  const std::string head = buf.substr(0, header_end);
  std::stringstream hs(head);
  std::string request_line;
  std::getline(hs, request_line);
  if (!request_line.empty() && request_line.back() == '\r')
    request_line.pop_back();

  HttpRequest req;
  {
    std::stringstream rl(request_line);
    std::string target;
    rl >> req.method >> target; // METHOD TARGET HTTP/1.1
    ParseTarget(target, &req);
  }

  std::size_t content_length = 0;
  std::string line;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
      value.erase(value.begin());
    for (auto& ch : name)
      ch = static_cast<char>(::tolower(ch));
    if (name == "content-length") {
      content_length = static_cast<std::size_t>(std::stoul(value));
    }
  }

  // Read the body (Content-Length bytes after the header block).
  std::string body = buf.substr(header_end + 4);
  while (body.size() < content_length) {
    ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0)
      break;
    body.append(chunk, static_cast<std::size_t>(n));
  }
  req.body = std::move(body);

  HttpResponse resp = handler_(req);

  std::string out =
      "HTTP/1.1 " + std::to_string(resp.status) + " " + ReasonPhrase(resp.status) + "\r\n";
  out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  out += "Content-Type: " + resp.content_type + "\r\n";
  for (const auto& [k, v] : resp.headers) {
    out += k + ": " + v + "\r\n";
  }
  out += "Connection: close\r\n\r\n";
  out += resp.body;

  WriteAll(fd, out);
  ::close(fd);
}

void HttpServer::Shutdown() {
  bool was_running = running_.exchange(false);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (was_running && accept_thread_.joinable()) {
    accept_thread_.join();
  }
  pool_.Shutdown();
}

} // namespace dos
