#ifndef DOS_NETWORK_RAFT_SERVER_H_
#define DOS_NETWORK_RAFT_SERVER_H_

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "consensus/raft_node.h"
#include "raft.grpc.pb.h"

namespace dos {

// gRPC front end for a RaftNode's inbound peer RPCs (spec Milestone 15). Peers
// call RequestVote/AppendEntries here; the service delegates straight to the
// node's thread-safe handlers.
class RaftServiceImpl final : public rpc::Raft::Service {
public:
  explicit RaftServiceImpl(consensus::RaftNode& node) : node_(node) {}

  grpc::Status RequestVote(grpc::ServerContext*, const rpc::RequestVoteRequest* req,
                           rpc::RequestVoteResponse* resp) override;
  grpc::Status AppendEntries(grpc::ServerContext*, const rpc::AppendEntriesRequest* req,
                             rpc::AppendEntriesResponse* resp) override;

private:
  consensus::RaftNode& node_;
};

class RaftServer {
public:
  explicit RaftServer(consensus::RaftNode& node) : service_(node) {}
  ~RaftServer() { Shutdown(); }

  RaftServer(const RaftServer&) = delete;
  RaftServer& operator=(const RaftServer&) = delete;

  bool Start(const std::string& address);
  int bound_port() const { return bound_port_; }
  void Shutdown();
  void Wait();

private:
  RaftServiceImpl service_;
  std::unique_ptr<grpc::Server> server_;
  int bound_port_ = 0;
};

} // namespace dos

#endif // DOS_NETWORK_RAFT_SERVER_H_
