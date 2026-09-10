#include "network/storage_node_server.h"

#include <grpcpp/grpcpp.h>

#include "network/proto_util.h"

namespace dos {

grpc::Status StorageNodeServiceImpl::Put(grpc::ServerContext*, const rpc::PutRequest* req,
                                         rpc::PutResponse* resp) {
  // Version 0 means "auto-assign" (single-node/local use); a non-zero version
  // is the coordinator-assigned version shared across replicas.
  StatusOr<ObjectMetadata> result =
      req->version() == 0 ? store_.Put(req->key(), req->data())
                          : store_.PutWithVersion(req->key(), req->data(), req->version());
  if (result.ok()) {
    resp->set_code(rpc::OK);
    ToProtoMeta(result.value(), resp->mutable_meta());
  } else {
    resp->set_code(ToProtoCode(result.status().code()));
    resp->set_message(result.status().message());
  }
  return grpc::Status::OK;
}

grpc::Status StorageNodeServiceImpl::Get(grpc::ServerContext*, const rpc::GetRequest* req,
                                         rpc::GetResponse* resp) {
  auto data = store_.Get(req->key());
  if (data.ok()) {
    resp->set_code(rpc::OK);
    resp->set_data(data.value());
    auto head = store_.Head(req->key());
    if (head.ok()) {
      ToProtoMeta(head.value(), resp->mutable_meta());
    }
  } else {
    resp->set_code(ToProtoCode(data.status().code()));
    resp->set_message(data.status().message());
  }
  return grpc::Status::OK;
}

grpc::Status StorageNodeServiceImpl::Head(grpc::ServerContext*, const rpc::HeadRequest* req,
                                          rpc::HeadResponse* resp) {
  auto head = store_.Head(req->key());
  if (head.ok()) {
    resp->set_code(rpc::OK);
    ToProtoMeta(head.value(), resp->mutable_meta());
  } else {
    resp->set_code(ToProtoCode(head.status().code()));
    resp->set_message(head.status().message());
  }
  return grpc::Status::OK;
}

grpc::Status StorageNodeServiceImpl::Delete(grpc::ServerContext*, const rpc::DeleteRequest* req,
                                            rpc::DeleteResponse* resp) {
  Status s = store_.Delete(req->key());
  resp->set_code(ToProtoCode(s.code()));
  if (!s.ok()) {
    resp->set_message(s.message());
  }
  return grpc::Status::OK;
}

bool StorageNodeServer::Start(const std::string& address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port_);
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  return server_ != nullptr && bound_port_ != 0;
}

void StorageNodeServer::Shutdown() {
  if (server_) {
    server_->Shutdown();
    server_.reset();
  }
}

void StorageNodeServer::Wait() {
  if (server_) {
    server_->Wait();
  }
}

} // namespace dos
