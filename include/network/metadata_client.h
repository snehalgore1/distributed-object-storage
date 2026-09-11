#ifndef DOS_NETWORK_METADATA_CLIENT_H_
#define DOS_NETWORK_METADATA_CLIENT_H_

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cluster/metadata_view.h"
#include "metadata.grpc.pb.h"

namespace dos {

// Client-side MetadataView backed by the remote Metadata gRPC service. Lets the
// coordinator/repairer treat a remote control plane exactly like an in-process
// one (spec Milestone 8).
class RemoteMetadataView : public MetadataView {
public:
  explicit RemoteMetadataView(const std::string& address,
                              std::chrono::milliseconds deadline = std::chrono::milliseconds(500));

  // Membership admin (not part of MetadataView; used to configure the cluster).
  Status AddNode(const std::string& id, const std::string& address);

  std::vector<std::string> PlacementFor(std::string_view key, std::size_t rf) override;
  Status RegisterObject(const ObjectLocation& loc) override;
  StatusOr<ObjectLocation> LookupObject(std::string_view key) override;
  Status RemoveObject(std::string_view key) override;
  StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view prefix) override;

private:
  void SetDeadline(grpc::ClientContext& ctx) const;

  std::string address_;
  std::chrono::milliseconds deadline_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<rpc::Metadata::Stub> stub_;
};

} // namespace dos

#endif // DOS_NETWORK_METADATA_CLIENT_H_
