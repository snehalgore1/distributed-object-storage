#ifndef DOS_CONSENSUS_IN_MEMORY_TRANSPORT_H_
#define DOS_CONSENSUS_IN_MEMORY_TRANSPORT_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "consensus/raft_transport.h"

namespace dos {
namespace consensus {

class RaftNode;

// In-process transport that dispatches RequestVote/AppendEntries straight into
// the target node's handlers — no sockets, no serialization delay. This is what
// makes the Raft tests deterministic and fast: failures are modeled by marking
// a node "down" (its inbound RPCs fail, mimicking an unreachable peer), rather
// than by racing real timeouts.
//
// One shared Registry maps node id -> RaftNode. Each node gets an Endpoint bound
// to its own id, which it hands to its RaftNode as the RaftTransport.
class InMemoryCluster {
public:
  void Register(const std::string& id, RaftNode* node);

  // Toggle a node's reachability. A downed node answers no inbound RPCs and its
  // own outbound RPCs fail, simulating a crash / network partition.
  void SetDown(const std::string& id, bool down);
  bool IsDown(const std::string& id) const;

  // Delivers an RPC to `target` unless either endpoint is down.
  bool DeliverRequestVote(const std::string& from, const std::string& target,
                          const rpc::RequestVoteRequest& req, rpc::RequestVoteResponse* resp);
  bool DeliverAppendEntries(const std::string& from, const std::string& target,
                            const rpc::AppendEntriesRequest& req, rpc::AppendEntriesResponse* resp);

  // A per-node view handed to one RaftNode as its outbound transport.
  std::unique_ptr<RaftTransport> EndpointFor(const std::string& id);

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, RaftNode*> nodes_;
  std::unordered_set<std::string> down_;
};

} // namespace consensus
} // namespace dos

#endif // DOS_CONSENSUS_IN_MEMORY_TRANSPORT_H_
