#ifndef DOS_CONSENSUS_RAFT_TRANSPORT_H_
#define DOS_CONSENSUS_RAFT_TRANSPORT_H_

#include <string>

#include "raft.pb.h"

namespace dos {
namespace consensus {

// Outbound peer RPCs, abstracted so the Raft core is independent of the wire.
// Two implementations exist: an in-memory transport for deterministic tests and
// a gRPC transport for a real multi-process cluster (spec Milestone 15).
//
// Calls are synchronous with a deadline. A return of false means the peer was
// unreachable / timed out (a normal, expected condition Raft tolerates); true
// means `resp` was populated by the peer.
class RaftTransport {
public:
  virtual ~RaftTransport() = default;

  virtual bool SendRequestVote(const std::string& peer, const rpc::RequestVoteRequest& req,
                               rpc::RequestVoteResponse* resp) = 0;
  virtual bool SendAppendEntries(const std::string& peer, const rpc::AppendEntriesRequest& req,
                                 rpc::AppendEntriesResponse* resp) = 0;
};

} // namespace consensus
} // namespace dos

#endif // DOS_CONSENSUS_RAFT_TRANSPORT_H_
