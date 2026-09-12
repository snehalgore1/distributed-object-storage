#include "consensus/in_memory_transport.h"

#include "consensus/raft_node.h"

namespace dos {
namespace consensus {
namespace {

// Outbound view bound to one node id; forwards to the shared cluster.
class Endpoint : public RaftTransport {
public:
  Endpoint(InMemoryCluster* cluster, std::string id) : cluster_(cluster), id_(std::move(id)) {}

  bool SendRequestVote(const std::string& peer, const rpc::RequestVoteRequest& req,
                       rpc::RequestVoteResponse* resp) override {
    return cluster_->DeliverRequestVote(id_, peer, req, resp);
  }
  bool SendAppendEntries(const std::string& peer, const rpc::AppendEntriesRequest& req,
                         rpc::AppendEntriesResponse* resp) override {
    return cluster_->DeliverAppendEntries(id_, peer, req, resp);
  }

private:
  InMemoryCluster* cluster_;
  std::string id_;
};

} // namespace

void InMemoryCluster::Register(const std::string& id, RaftNode* node) {
  std::lock_guard<std::mutex> lock(mu_);
  nodes_[id] = node;
}

void InMemoryCluster::SetDown(const std::string& id, bool down) {
  std::lock_guard<std::mutex> lock(mu_);
  if (down) {
    down_.insert(id);
  } else {
    down_.erase(id);
  }
}

bool InMemoryCluster::IsDown(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return down_.count(id) != 0;
}

bool InMemoryCluster::DeliverRequestVote(const std::string& from, const std::string& target,
                                         const rpc::RequestVoteRequest& req,
                                         rpc::RequestVoteResponse* resp) {
  RaftNode* node = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (down_.count(from) != 0 || down_.count(target) != 0) {
      return false; // sender or receiver unreachable
    }
    auto it = nodes_.find(target);
    if (it == nodes_.end()) {
      return false;
    }
    node = it->second;
  }
  node->HandleRequestVote(req, resp); // handler is thread-safe on its own mutex
  return true;
}

bool InMemoryCluster::DeliverAppendEntries(const std::string& from, const std::string& target,
                                           const rpc::AppendEntriesRequest& req,
                                           rpc::AppendEntriesResponse* resp) {
  RaftNode* node = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (down_.count(from) != 0 || down_.count(target) != 0) {
      return false;
    }
    auto it = nodes_.find(target);
    if (it == nodes_.end()) {
      return false;
    }
    node = it->second;
  }
  node->HandleAppendEntries(req, resp);
  return true;
}

std::unique_ptr<RaftTransport> InMemoryCluster::EndpointFor(const std::string& id) {
  return std::make_unique<Endpoint>(this, id);
}

} // namespace consensus
} // namespace dos
