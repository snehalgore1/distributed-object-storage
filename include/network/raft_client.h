#ifndef DOS_NETWORK_RAFT_CLIENT_H_
#define DOS_NETWORK_RAFT_CLIENT_H_

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "consensus/raft_transport.h"
#include "raft.grpc.pb.h"

namespace dos {

// gRPC-backed RaftTransport (spec Milestone 15). Maps peer ids to addresses and
// keeps one lazily-created stub per peer. A bounded deadline means an
// unreachable peer fails fast (returning false) rather than stalling elections
// or heartbeats — exactly the "peer down" signal Raft is built to tolerate.
class GrpcRaftTransport : public consensus::RaftTransport {
public:
  explicit GrpcRaftTransport(std::unordered_map<std::string, std::string> peer_addresses,
                             std::chrono::milliseconds deadline = std::chrono::milliseconds(100));

  // Registers/updates a peer's address before the node starts. Useful when peer
  // ports are only known after their servers bind (e.g., ephemeral ports).
  void SetPeer(const std::string& peer_id, const std::string& address);

  bool SendRequestVote(const std::string& peer, const rpc::RequestVoteRequest& req,
                       rpc::RequestVoteResponse* resp) override;
  bool SendAppendEntries(const std::string& peer, const rpc::AppendEntriesRequest& req,
                         rpc::AppendEntriesResponse* resp) override;

private:
  rpc::Raft::Stub* StubFor(const std::string& peer);

  std::unordered_map<std::string, std::string> addresses_; // peer id -> host:port
  std::chrono::milliseconds deadline_;

  std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<rpc::Raft::Stub>> stubs_;
};

} // namespace dos

#endif // DOS_NETWORK_RAFT_CLIENT_H_
