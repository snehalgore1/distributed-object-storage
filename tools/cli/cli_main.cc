// Small command-line client for the object store, talking to the HTTP gateway.
//
// Usage:
//   dos_cli [--gateway HOST:PORT] put <key> <value|@file>
//   dos_cli [--gateway HOST:PORT] get <key>
//   dos_cli [--gateway HOST:PORT] head <key>
//   dos_cli [--gateway HOST:PORT] delete <key>
//   dos_cli [--gateway HOST:PORT] list [prefix]

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "network/http_client.h"

namespace {

std::pair<std::string, int> SplitHostPort(const std::string& hp) {
  const auto c = hp.rfind(':');
  if (c == std::string::npos)
    return {hp, 8080};
  return {hp.substr(0, c), std::atoi(hp.substr(c + 1).c_str())};
}

std::string ReadValueArg(const std::string& arg) {
  if (!arg.empty() && arg[0] == '@') { // @file -> file contents
    std::ifstream in(arg.substr(1), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  return arg;
}

int Usage() {
  std::cerr << "usage: dos_cli [--gateway HOST:PORT] <put|get|head|delete|list> ...\n";
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string gateway = "127.0.0.1:8080";
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--gateway") {
      gateway = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
      break;
    }
  }
  if (args.empty())
    return Usage();

  const auto [host, port] = SplitHostPort(gateway);
  dos::HttpClient client(host, port);
  const std::string& cmd = args[0];

  dos::StatusOr<dos::HttpClientResponse> resp = dos::Status::InvalidArgument("no request");
  if (cmd == "put" && args.size() == 3) {
    resp = client.Request("PUT", "/objects/" + args[1], ReadValueArg(args[2]));
  } else if (cmd == "get" && args.size() == 2) {
    resp = client.Request("GET", "/objects/" + args[1]);
  } else if (cmd == "head" && args.size() == 2) {
    resp = client.Request("HEAD", "/objects/" + args[1]);
  } else if (cmd == "delete" && args.size() == 2) {
    resp = client.Request("DELETE", "/objects/" + args[1]);
  } else if (cmd == "list") {
    resp = client.Request("GET", args.size() >= 2 ? "/objects?prefix=" + args[1] : "/objects");
  } else {
    return Usage();
  }

  if (!resp.ok()) {
    std::cerr << "error: " << resp.status().ToString() << "\n";
    return 1;
  }
  const auto& r = resp.value();
  if (!r.body.empty()) {
    std::cout << r.body;
    if (r.body.back() != '\n')
      std::cout << "\n";
  }
  std::cerr << "HTTP " << r.status << "\n";
  return (r.status >= 200 && r.status < 300) ? 0 : 1;
}
