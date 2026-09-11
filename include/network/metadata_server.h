#ifndef DOS_NETWORK_METADATA_SERVER_H_
#define DOS_NETWORK_METADATA_SERVER_H_

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "cluster/metadata_repository.h"
#include "metadata.grpc.pb.h"

namespace dos {

// gRPC front end for the control plane. Wraps a MetadataRepository and exposes
// placement, object-location register/lookup/remove/list, and membership.
class MetadataServiceImpl final : public rpc::Metadata::Service {
public:
  explicit MetadataServiceImpl(MetadataRepository& repo) : repo_(repo) {}

  grpc::Status Placement(grpc::ServerContext*, const rpc::PlacementRequest*,
                         rpc::PlacementResponse*) override;
  grpc::Status RegisterObject(grpc::ServerContext*, const rpc::ObjectLocation*,
                              rpc::MetaAck*) override;
  grpc::Status LookupObject(grpc::ServerContext*, const rpc::KeyRequest*,
                            rpc::LookupResponse*) override;
  grpc::Status RemoveObject(grpc::ServerContext*, const rpc::KeyRequest*, rpc::MetaAck*) override;
  grpc::Status ListObjects(grpc::ServerContext*, const rpc::PrefixRequest*,
                           rpc::ListObjectsResponse*) override;
  grpc::Status AddNode(grpc::ServerContext*, const rpc::NodeDesc*, rpc::MetaAck*) override;
  grpc::Status ListNodes(grpc::ServerContext*, const rpc::NodesRequest*,
                         rpc::ListNodesResponse*) override;

private:
  MetadataRepository& repo_;
};

class MetadataServer {
public:
  explicit MetadataServer(MetadataRepository& repo) : service_(repo) {}
  ~MetadataServer() { Shutdown(); }

  MetadataServer(const MetadataServer&) = delete;
  MetadataServer& operator=(const MetadataServer&) = delete;

  bool Start(const std::string& address);
  int bound_port() const { return bound_port_; }
  void Shutdown();
  void Wait();

private:
  MetadataServiceImpl service_;
  std::unique_ptr<grpc::Server> server_;
  int bound_port_ = 0;
};

} // namespace dos

#endif // DOS_NETWORK_METADATA_SERVER_H_
