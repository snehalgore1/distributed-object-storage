#include "network/raft_client.h"

namespace dos {

GrpcRaftTransport::GrpcRaftTransport(std::unordered_map<std::string, std::string> peer_addresses,
                                     std::chrono::milliseconds deadline)
    : addresses_(std::move(peer_addresses)), deadline_(deadline) {}

void GrpcRaftTransport::SetPeer(const std::string& peer_id, const std::string& address) {
  std::lock_guard<std::mutex> lock(mu_);
  addresses_[peer_id] = address;
  stubs_.erase(peer_id); // drop any stale stub bound to a previous address
}

rpc::Raft::Stub* GrpcRaftTransport::StubFor(const std::string& peer) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = stubs_.find(peer);
  if (it != stubs_.end()) {
    return it->second.get();
  }
  auto addr = addresses_.find(peer);
  if (addr == addresses_.end()) {
    return nullptr; // unknown peer
  }
  auto channel = grpc::CreateChannel(addr->second, grpc::InsecureChannelCredentials());
  auto stub = rpc::Raft::NewStub(channel);
  rpc::Raft::Stub* raw = stub.get();
  stubs_[peer] = std::move(stub);
  return raw;
}

bool GrpcRaftTransport::SendRequestVote(const std::string& peer, const rpc::RequestVoteRequest& req,
                                        rpc::RequestVoteResponse* resp) {
  rpc::Raft::Stub* stub = StubFor(peer);
  if (stub == nullptr) {
    return false;
  }
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + deadline_);
  return stub->RequestVote(&ctx, req, resp).ok();
}

bool GrpcRaftTransport::SendAppendEntries(const std::string& peer,
                                          const rpc::AppendEntriesRequest& req,
                                          rpc::AppendEntriesResponse* resp) {
  rpc::Raft::Stub* stub = StubFor(peer);
  if (stub == nullptr) {
    return false;
  }
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + deadline_);
  return stub->AppendEntries(&ctx, req, resp).ok();
}

} // namespace dos
