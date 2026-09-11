# Architecture

> Status: current through **Milestone 8** — the control/data-plane split is real.
> Later layers (cache/HTTP gateway, observability, deployment) are planned.

## Design principle: separate control plane from data plane

- The **control/metadata plane** answers *"where does this object live, and
  which version is current?"*
- The **data plane** (storage nodes) answers *"give me the bytes."*

**Milestone 8** makes this split concrete. The control plane is the
[`Metadata`](../proto/metadata.proto) gRPC service — a `MetadataRepository`
(membership + consistent-hash placement + the object→replica-set map) behind the
`MetadataView` interface. The coordinator holds no placement itself: it asks the
metadata service *where* to write, writes bytes to the data-plane `StorageNode`
services under a quorum, then *registers* the resulting replica set back with the
metadata service. Reads ask the metadata service for the replica set, then fetch
bytes from a storage node. **Object payloads never pass through the metadata
service** — its `ObjectLocation` records carry version, checksum, size, and
replica ids only.

`MetadataView` has two implementations, so the same coordinator code runs
against an in-process repository (tests) or the remote gRPC service
(`RemoteMetadataView`) unchanged. Each storage node still keeps its own local
`SqliteMetadataStore` for the objects it physically holds.

## Target topology

```
        Client
          |
   Gateway (HTTP/CLI -> gRPC)      [Milestone 9]
          |
   Metadata / Placement           [Milestone 8]  object -> replica set
          |
    +-----+-----+
    |     |     |
  Node A Node B Node C            [Milestones 3-7]
    |     |     |
    +-----+-----+
   replicated data (RF=3, quorum writes)
```

Each storage node = filesystem + WAL + checksums + local metadata + metrics.

## Components

| Component | Header | Responsibility |
|-----------|--------|----------------|
| **Data plane** | | |
| `ObjectStore` / `LocalObjectStore` | `storage/` | PUT/GET/HEAD/DELETE/LIST; atomic durable writes + WAL |
| `SqliteMetadataStore` | `storage/sqlite_metadata_store.h` | per-node metadata for objects that node holds |
| `Wal` | `storage/wal.h` | write-ahead log + crash recovery |
| `StorageNode` service | `network/storage_node_*` | gRPC data-plane server/client |
| **Control plane** | | |
| `MetadataView` | `cluster/metadata_view.h` | placement + object→replica-set interface |
| `MetadataRepository` | `cluster/metadata_repository.h` | in-process authoritative impl (membership, ring, locations) |
| `Metadata` service | `network/metadata_*` | gRPC control-plane server/client (`RemoteMetadataView`) |
| `ConsistentHashRing` / `ClusterMap` | `cluster/` | virtual-node placement, membership |
| `FailureDetector` | `cluster/failure_detector.h` | heartbeat state machine |
| **Orchestration** | | |
| `Coordinator` | `network/coordinator.h` | quorum write / fallback read across replicas |
| `Repairer` | `network/repairer.h` | anti-entropy repair from authoritative metadata |
| **Common** | | |
| `Sha256` / `hash` / `digest` | `common/` | checksums, stable 64-bit hash, key→path |
| `ThreadPool` | `common/thread_pool.h` | bounded-queue worker pool |
| `Status` / `StatusOr` | `common/status.h` | error propagation without exceptions |

See [storage-engine.md](storage-engine.md), [protocol.md](protocol.md), and
[failure-model.md](failure-model.md) for detail.

## Roadmap

✅ M0–M8 (core + metadata-service split). Remaining: M9 cache + HTTP gateway ·
M10 observability · M11 Docker · M12 Kubernetes · M13 performance · M14 chaos ·
M15 (optional) Raft.
