#ifndef DOS_NETWORK_METADATA_SERVER_H_
#define DOS_NETWORK_METADATA_SERVER_H_

#include <grpcpp/grpcpp.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cluster/control_plane.h"
#include "metadata.grpc.pb.h"
#include "network/metadata_client.h"

namespace dos {

// gRPC front end for the control plane. Wraps any ControlPlane (a single-node
// MetadataRepository or a Raft-replicated RaftMetadataRepository) and exposes
// placement, object-location register/lookup/remove/list, and membership.
//
// Reads are served locally. A mutation received on a Raft follower is forwarded
// to the current leader (etcd-style) so clients can target any replica; if no
// leader is known it is rejected with UNAVAILABLE. A single-node control plane
// reports itself leader and always serves locally.
class MetadataServiceImpl final : public rpc::Metadata::Service {
public:
  explicit MetadataServiceImpl(ControlPlane& repo) : repo_(repo) {}

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
  enum class Route { kLocal, kForward, kReject };
  // Decides where a mutation should run; on kForward, fills *leader_address.
  Route ResolveMutation(std::string* leader_address);
  // Returns a cached client to the leader's metadata service.
  RemoteMetadataView* ForwardClient(const std::string& address);

  ControlPlane& repo_;

  std::mutex forward_mu_;
  std::unordered_map<std::string, std::unique_ptr<RemoteMetadataView>> forward_clients_;
};

class MetadataServer {
public:
  explicit MetadataServer(ControlPlane& repo) : service_(repo) {}
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
