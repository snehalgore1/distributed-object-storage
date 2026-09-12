#include "network/raft_server.h"

namespace dos {

grpc::Status RaftServiceImpl::RequestVote(grpc::ServerContext*, const rpc::RequestVoteRequest* req,
                                          rpc::RequestVoteResponse* resp) {
  node_.HandleRequestVote(*req, resp);
  return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::AppendEntries(grpc::ServerContext*,
                                            const rpc::AppendEntriesRequest* req,
                                            rpc::AppendEntriesResponse* resp) {
  node_.HandleAppendEntries(*req, resp);
  return grpc::Status::OK;
}

bool RaftServer::Start(const std::string& address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port_);
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  return server_ != nullptr && bound_port_ != 0;
}

void RaftServer::Shutdown() {
  if (server_) {
    server_->Shutdown();
    server_.reset();
  }
}

void RaftServer::Wait() {
  if (server_) {
    server_->Wait();
  }
}

} // namespace dos
