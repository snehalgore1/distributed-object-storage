#ifndef DOS_NETWORK_STORAGE_NODE_CLIENT_H_
#define DOS_NETWORK_STORAGE_NODE_CLIENT_H_

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common/status.h"
#include "storage.grpc.pb.h"
#include "storage/object_metadata.h"

namespace dos {

// Client-side wrapper around one storage node's gRPC stub. RPCs carry a bounded
// deadline so a slow/dead node cannot block the coordinator indefinitely
// (spec Milestone 6 tunes these; a safe default is set here).
class StorageNodeClient {
public:
  explicit StorageNodeClient(const std::string& address,
                             std::chrono::milliseconds deadline = std::chrono::milliseconds(500));

  // Writes at the coordinator-assigned `version`.
  StatusOr<ObjectMetadata> Put(const std::string& key, const std::string& data, uint64_t version);

  // Conditional, idempotent write: enforce `expected_version` and dedup by
  // `request_id` (spec Milestone 5).
  StatusOr<ObjectMetadata> PutConditional(const std::string& key, const std::string& data,
                                          uint64_t expected_version, const std::string& request_id);
  StatusOr<std::string> Get(const std::string& key);
  StatusOr<ObjectMetadata> Head(const std::string& key);
  Status Delete(const std::string& key);

  // Liveness probe; returns the node's live object count on success.
  StatusOr<uint64_t> Health();
  // Enumerate objects (for repair).
  StatusOr<std::vector<ObjectMetadata>> List(const std::string& prefix);

  const std::string& address() const { return address_; }

private:
  void SetDeadline(grpc::ClientContext& ctx) const;

  std::string address_;
  std::chrono::milliseconds deadline_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<rpc::StorageNode::Stub> stub_;
};

} // namespace dos

#endif // DOS_NETWORK_STORAGE_NODE_CLIENT_H_
