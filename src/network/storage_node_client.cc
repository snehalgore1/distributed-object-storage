#include "network/storage_node_client.h"

#include "network/proto_util.h"

namespace dos {

StorageNodeClient::StorageNodeClient(const std::string& address, std::chrono::milliseconds deadline)
    : address_(address), deadline_(deadline),
      channel_(grpc::CreateChannel(address, grpc::InsecureChannelCredentials())),
      stub_(rpc::StorageNode::NewStub(channel_)) {}

void StorageNodeClient::SetDeadline(grpc::ClientContext& ctx) const {
  ctx.set_deadline(std::chrono::system_clock::now() + deadline_);
}

StatusOr<ObjectMetadata> StorageNodeClient::Put(const std::string& key, const std::string& data,
                                                uint64_t version) {
  rpc::PutRequest req;
  req.set_key(key);
  req.set_data(data);
  req.set_version(version);
  rpc::PutResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->Put(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("Put RPC to " + address_ + " failed: " + s.error_message());
  }
  if (resp.code() != rpc::OK) {
    return Status(FromProtoCode(resp.code()), resp.message());
  }
  return FromProtoMeta(resp.meta());
}

StatusOr<std::string> StorageNodeClient::Get(const std::string& key) {
  rpc::GetRequest req;
  req.set_key(key);
  rpc::GetResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->Get(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("Get RPC to " + address_ + " failed: " + s.error_message());
  }
  if (resp.code() != rpc::OK) {
    return Status(FromProtoCode(resp.code()), resp.message());
  }
  return resp.data();
}

StatusOr<ObjectMetadata> StorageNodeClient::Head(const std::string& key) {
  rpc::HeadRequest req;
  req.set_key(key);
  rpc::HeadResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->Head(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("Head RPC to " + address_ + " failed: " + s.error_message());
  }
  if (resp.code() != rpc::OK) {
    return Status(FromProtoCode(resp.code()), resp.message());
  }
  return FromProtoMeta(resp.meta());
}

Status StorageNodeClient::Delete(const std::string& key) {
  rpc::DeleteRequest req;
  req.set_key(key);
  rpc::DeleteResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);
  grpc::Status s = stub_->Delete(&ctx, req, &resp);
  if (!s.ok()) {
    return Status::Unavailable("Delete RPC to " + address_ + " failed: " + s.error_message());
  }
  return Status(FromProtoCode(resp.code()), resp.message());
}

} // namespace dos
