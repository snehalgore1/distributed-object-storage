#ifndef DOS_NETWORK_STORAGE_NODE_SERVER_H_
#define DOS_NETWORK_STORAGE_NODE_SERVER_H_

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "storage.grpc.pb.h"
#include "storage/object_store.h"

namespace dos {

// gRPC service that serves one node's local object store (data plane).
class StorageNodeServiceImpl final : public rpc::StorageNode::Service {
public:
  explicit StorageNodeServiceImpl(ObjectStore& store) : store_(store) {}

  grpc::Status Put(grpc::ServerContext*, const rpc::PutRequest*, rpc::PutResponse*) override;
  grpc::Status Get(grpc::ServerContext*, const rpc::GetRequest*, rpc::GetResponse*) override;
  grpc::Status Head(grpc::ServerContext*, const rpc::HeadRequest*, rpc::HeadResponse*) override;
  grpc::Status Delete(grpc::ServerContext*, const rpc::DeleteRequest*,
                      rpc::DeleteResponse*) override;

private:
  ObjectStore& store_;
};

// Owns a running gRPC server bound to a single storage node's store.
class StorageNodeServer {
public:
  explicit StorageNodeServer(ObjectStore& store) : service_(store) {}
  ~StorageNodeServer() { Shutdown(); }

  StorageNodeServer(const StorageNodeServer&) = delete;
  StorageNodeServer& operator=(const StorageNodeServer&) = delete;

  // Binds and starts serving on `address` (e.g. "0.0.0.0:9001", or
  // "127.0.0.1:0" to auto-pick a free port). Returns false on bind failure.
  bool Start(const std::string& address);

  // The actually-bound port (useful when starting on port 0). 0 if not started.
  int bound_port() const { return bound_port_; }

  void Shutdown();
  void Wait(); // blocks until the server shuts down

private:
  StorageNodeServiceImpl service_;
  std::unique_ptr<grpc::Server> server_;
  int bound_port_ = 0;
};

} // namespace dos

#endif // DOS_NETWORK_STORAGE_NODE_SERVER_H_
