#include "network/metadata_client.h"

#include "network/proto_util.h"

namespace dos {
namespace {

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

RemoteMetadataView::RemoteMetadataView(const std::string& address,
                                       std::chrono::milliseconds deadline)
    : address_(address), deadline_(deadline),
      channel_(grpc::CreateChannel(address, grpc::InsecureChannelCredentials())),
      stub_(rpc::Metadata::NewStub(channel_)) {}

void RemoteMetadataView::SetDeadline(grpc::ClientContext& ctx) const {
  ctx.set_deadline(std::chrono::system_clock::now() + deadline_);
}

Status RemoteMetadataView::AddNode(const std::string& id, const std::string& address) {
  rpc::NodeDesc req;
  req.set_id(id);
  req.set_address(address);
  rpc::MetaAck resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->AddNode(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("AddNode RPC failed: " + s.error_message());
  }
  return Status(FromProtoCode(resp.code()), resp.message());
}

StatusOr<std::vector<NodeInfo>> RemoteMetadataView::ListNodes() {
  rpc::NodesRequest req;
  rpc::ListNodesResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->ListNodes(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("ListNodes RPC failed: " + s.error_message());
  }
  std::vector<NodeInfo> out;
  for (const auto& n : resp.nodes()) {
    NodeInfo info;
    info.id = n.id();
    info.address = n.address();
    out.push_back(info);
  }
  return out;
}

std::vector<std::string> RemoteMetadataView::PlacementFor(std::string_view key, std::size_t rf) {
  rpc::PlacementRequest req;
  req.set_key(std::string(key));
  req.set_replication_factor(static_cast<uint32_t>(rf));
  rpc::PlacementResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->Placement(&ctx, req, &resp);
  std::vector<std::string> out;
  if (s.ok()) {
    for (const auto& id : resp.node_ids()) {
      out.push_back(id);
    }
  }
  return out;
}

Status RemoteMetadataView::RegisterObject(const ObjectLocation& loc) {
  rpc::ObjectLocation req;
  req.set_key(loc.key);
  req.set_version(loc.version);
  req.set_checksum(loc.checksum);
  req.set_size(loc.size);
  req.set_deleted(loc.deleted);
  for (const auto& r : loc.replicas) {
    req.add_replicas(r);
  }
  rpc::MetaAck resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->RegisterObject(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("RegisterObject RPC failed: " + s.error_message());
  }
  return Status(FromProtoCode(resp.code()), resp.message());
}

StatusOr<ObjectLocation> RemoteMetadataView::LookupObject(std::string_view key) {
  rpc::KeyRequest req;
  req.set_key(std::string(key));
  rpc::LookupResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->LookupObject(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("LookupObject RPC failed: " + s.error_message());
  }
  if (resp.code() != rpc::OK) {
    return Status(FromProtoCode(resp.code()), resp.message());
  }
  return FromProtoLocation(resp.location());
}

Status RemoteMetadataView::RemoveObject(std::string_view key) {
  rpc::KeyRequest req;
  req.set_key(std::string(key));
  rpc::MetaAck resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->RemoveObject(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("RemoveObject RPC failed: " + s.error_message());
  }
  return Status(FromProtoCode(resp.code()), resp.message());
}

StatusOr<std::vector<ObjectLocation>> RemoteMetadataView::ListObjects(std::string_view prefix) {
  rpc::PrefixRequest req;
  req.set_prefix(std::string(prefix));
  rpc::ListObjectsResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->ListObjects(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("ListObjects RPC failed: " + s.error_message());
  }
  std::vector<ObjectLocation> out;
  for (const auto& loc : resp.objects()) {
    out.push_back(FromProtoLocation(loc));
  }
  return out;
}

} // namespace dos
