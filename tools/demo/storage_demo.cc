// Minimal end-to-end smoke tool for the single-node storage engine.
// Usage: storage_demo <root-dir>
//
// Exercises PUT -> GET -> HEAD -> LIST -> DELETE against a LocalObjectStore
// rooted at <root-dir>, printing what happens at each step.

#include <cstdlib>
#include <iostream>
#include <string>

#include "storage/local_object_store.h"

using dos::LocalObjectStore;

namespace {

int Fail(const std::string& what, const dos::Status& s) {
  std::cerr << "FAIL " << what << ": " << s.ToString() << "\n";
  return 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <root-dir>\n";
    return 2;
  }
  const std::string root = argv[1];

  auto store_or = LocalObjectStore::Open(root);
  if (!store_or.ok()) {
    return Fail("open", store_or.status());
  }
  auto store = std::move(store_or).value();

  const std::string key = "greeting/hello.txt";
  const std::string payload = "hello, distributed world";

  auto put = store->Put(key, payload);
  if (!put.ok()) {
    return Fail("put", put.status());
  }
  std::cout << "PUT   key=" << key << " version=" << put.value().version
            << " size=" << put.value().size << " sha256=" << put.value().checksum << "\n";

  auto got = store->Get(key);
  if (!got.ok()) {
    return Fail("get", got.status());
  }
  std::cout << "GET   " << got.value().size() << " bytes: \"" << got.value() << "\"\n";
  std::cout << "      byte-identical? " << (got.value() == payload ? "yes" : "NO") << "\n";

  auto head = store->Head(key);
  if (!head.ok()) {
    return Fail("head", head.status());
  }
  std::cout << "HEAD  version=" << head.value().version << " deleted=" << head.value().deleted
            << "\n";

  auto list = store->List("");
  if (!list.ok()) {
    return Fail("list", list.status());
  }
  std::cout << "LIST  " << list.value().size() << " object(s)\n";

  auto del = store->Delete(key);
  if (!del.ok()) {
    return Fail("delete", del);
  }
  auto after = store->Get(key);
  std::cout << "DELETE then GET -> " << after.status().ToString() << " (expected NOT_FOUND)\n";

  std::cout << "OK\n";
  return 0;
}
