#include "network/metadata_server.h"

#include "network/proto_util.h"

namespace dos {
namespace {

void ToProtoLocation(const ObjectLocation& loc, rpc::ObjectLocation* out) {
  out->set_key(loc.key);
  out->set_version(loc.version);
  out->set_checksum(loc.checksum);
  out->set_size(loc.size);
  out->set_deleted(loc.deleted);
  for (const auto& r : loc.replicas) {
    out->add_replicas(r);
  }
}

ObjectLocation FromProtoLocation(const rpc::ObjectLocation& in) {
  ObjectLocation loc;
  loc.key = in.key();
  loc.version = in.version();
  loc.checksum = in.checksum();
  loc.size = in.size();
  loc.deleted = in.deleted();
  for (const auto& r : in.replicas()) {
    loc.replicas.push_back(r);
  }
  return loc;
}

} // namespace

grpc::Status MetadataServiceImpl::Placement(grpc::ServerContext*, const rpc::PlacementRequest* req,
                                            rpc::PlacementResponse* resp) {
  auto placement = repo_.PlacementFor(req->key(), req->replication_factor());
  resp->set_code(rpc::OK);
  for (const auto& id : placement) {
    resp->add_node_ids(id);
  }
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::RegisterObject(grpc::ServerContext*,
                                                 const rpc::ObjectLocation* req,
                                                 rpc::MetaAck* resp) {
  Status s = repo_.RegisterObject(FromProtoLocation(*req));
  resp->set_code(ToProtoCode(s.code()));
  if (!s.ok())
    resp->set_message(s.message());
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::LookupObject(grpc::ServerContext*, const rpc::KeyRequest* req,
                                               rpc::LookupResponse* resp) {
  auto loc = repo_.LookupObject(req->key());
  if (!loc.ok()) {
    resp->set_code(ToProtoCode(loc.status().code()));
    resp->set_message(loc.status().message());
    return grpc::Status::OK;
  }
  resp->set_code(rpc::OK);
  ToProtoLocation(loc.value(), resp->mutable_location());
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::RemoveObject(grpc::ServerContext*, const rpc::KeyRequest* req,
                                               rpc::MetaAck* resp) {
  Status s = repo_.RemoveObject(req->key());
  resp->set_code(ToProtoCode(s.code()));
  if (!s.ok())
    resp->set_message(s.message());
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::ListObjects(grpc::ServerContext*, const rpc::PrefixRequest* req,
                                              rpc::ListObjectsResponse* resp) {
  auto objs = repo_.ListObjects(req->prefix());
  resp->set_code(rpc::OK);
  if (objs.ok()) {
    for (const auto& loc : objs.value()) {
      ToProtoLocation(loc, resp->add_objects());
    }
  }
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::AddNode(grpc::ServerContext*, const rpc::NodeDesc* req,
                                          rpc::MetaAck* resp) {
  NodeInfo info;
  info.id = req->id();
  info.address = req->address();
  repo_.AddNode(info);
  resp->set_code(rpc::OK);
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::ListNodes(grpc::ServerContext*, const rpc::NodesRequest*,
                                            rpc::ListNodesResponse* resp) {
  resp->set_code(rpc::OK);
  for (const auto& n : repo_.Nodes()) {
    auto* d = resp->add_nodes();
    d->set_id(n.id);
    d->set_address(n.address);
  }
  return grpc::Status::OK;
}

bool MetadataServer::Start(const std::string& address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port_);
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  return server_ != nullptr && bound_port_ != 0;
}

void MetadataServer::Shutdown() {
  if (server_) {
    server_->Shutdown();
    server_.reset();
  }
}

void MetadataServer::Wait() {
  if (server_)
    server_->Wait();
}

} // namespace dos
