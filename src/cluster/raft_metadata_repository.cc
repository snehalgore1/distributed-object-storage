#include "cluster/raft_metadata_repository.h"

#include "consensus/raft_node.h"
#include "metadata.pb.h"

namespace dos {
namespace {

// Local ObjectLocation <-> proto converters (the codebase keeps these per-file;
// see metadata_server.cc / metadata_client.cc).
void ToProtoLocation(const ObjectLocation& loc, rpc::ObjectLocation* out) {
  out->set_key(loc.key);
  out->set_version(loc.version);
  out->set_checksum(loc.checksum);
  out->set_size(loc.size);
  out->set_deleted(loc.deleted);
  for (const std::string& r : loc.replicas) {
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
  for (const std::string& r : in.replicas()) {
    loc.replicas.push_back(r);
  }
  return loc;
}

} // namespace

void RaftMetadataRepository::Apply(const consensus::LogEntry& entry) {
  rpc::MetadataCommand cmd;
  if (!cmd.ParseFromString(entry.command)) {
    return; // malformed entries are impossible in practice; ignore defensively
  }
  switch (cmd.op_case()) {
  case rpc::MetadataCommand::kRegisterObject:
    inner_.RegisterObject(FromProtoLocation(cmd.register_object()));
    break;
  case rpc::MetadataCommand::kRemoveObject:
    inner_.RemoveObject(cmd.remove_object());
    break;
  case rpc::MetadataCommand::kAddNode: {
    NodeInfo info;
    info.id = cmd.add_node().id();
    info.address = cmd.add_node().address();
    inner_.AddNode(info);
    break;
  }
  case rpc::MetadataCommand::OP_NOT_SET:
    break;
  }
}

Status RaftMetadataRepository::ProposeAndWait(const std::string& command) {
  if (raft_ == nullptr) {
    return Status::Unavailable("raft not attached");
  }
  const uint64_t term = raft_->current_term();
  StatusOr<uint64_t> index = raft_->Propose(command);
  if (!index.ok()) {
    return index.status(); // not the leader
  }
  return raft_->WaitApplied(index.value(), term, commit_timeout_);
}

void RaftMetadataRepository::AddNode(const NodeInfo& node) {
  rpc::MetadataCommand cmd;
  rpc::NodeDesc* desc = cmd.mutable_add_node();
  desc->set_id(node.id);
  desc->set_address(node.address);
  ProposeAndWait(cmd.SerializeAsString());
}

Status RaftMetadataRepository::RegisterObject(const ObjectLocation& loc) {
  rpc::MetadataCommand cmd;
  ToProtoLocation(loc, cmd.mutable_register_object());
  return ProposeAndWait(cmd.SerializeAsString());
}

Status RaftMetadataRepository::RemoveObject(std::string_view key) {
  rpc::MetadataCommand cmd;
  cmd.set_remove_object(std::string(key));
  return ProposeAndWait(cmd.SerializeAsString());
}

Leadership RaftMetadataRepository::leadership() const {
  if (raft_ == nullptr || raft_->is_leader()) {
    return Leadership::kLeader;
  }
  return raft_->leader_id().empty() ? Leadership::kNoLeader : Leadership::kFollower;
}

std::string RaftMetadataRepository::leader_forward_address() const {
  if (raft_ == nullptr) {
    return "";
  }
  auto it = peer_meta_addr_.find(raft_->leader_id());
  return it == peer_meta_addr_.end() ? "" : it->second;
}

// Reads are served from locally-applied state.
std::vector<NodeInfo> RaftMetadataRepository::Nodes() const { return inner_.Nodes(); }

std::vector<std::string> RaftMetadataRepository::PlacementFor(std::string_view key,
                                                              std::size_t rf) {
  return inner_.PlacementFor(key, rf);
}

StatusOr<ObjectLocation> RaftMetadataRepository::LookupObject(std::string_view key) {
  return inner_.LookupObject(key);
}

StatusOr<std::vector<ObjectLocation>> RaftMetadataRepository::ListObjects(std::string_view prefix) {
  return inner_.ListObjects(prefix);
}

} // namespace dos
