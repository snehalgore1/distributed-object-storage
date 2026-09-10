# Architecture

> Status: this document describes the target architecture. The current codebase
> implements **Milestone 0** (build/test foundation) and **Milestone 1**
> (single-node storage engine). Distributed layers are planned.

## Design principle: separate control plane from data plane

- The **control/metadata plane** answers *"where does this object live, and
  which version is current?"*
- The **data plane** (storage nodes) answers *"give me the bytes."*

In Milestone 1 both live in one process: `LocalObjectStore` (data plane) writing
payloads to the filesystem, and `SqliteMetadataStore` (metadata plane) behind
the `MetadataStore` interface. That interface is the seam along which the two
planes are split into separate services in Milestone 8.

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

## Current components (M0 + M1)

| Component            | Header                                | Responsibility                              |
|----------------------|---------------------------------------|---------------------------------------------|
| `ObjectStore`        | `include/storage/object_store.h`      | PUT/GET/HEAD/DELETE/LIST contract           |
| `LocalObjectStore`   | `include/storage/local_object_store.h`| Filesystem-backed store, atomic durable PUT |
| `MetadataStore`      | `include/storage/metadata_store.h`    | Metadata persistence interface (the seam)   |
| `SqliteMetadataStore`| `include/storage/sqlite_metadata_store.h` | SQLite implementation of the above      |
| `Sha256` / `digest`  | `include/common/`                     | Content checksums and key→path placement    |
| `Status` / `StatusOr`| `include/common/status.h`             | Error propagation without exceptions        |

See [storage-engine.md](storage-engine.md) for the durability contract and
crash behavior.

## Roadmap

M2 concurrency (thread pool, sharded locks) · M3 cluster + consistent hashing ·
M4 replication + quorum · M5 versioning/idempotency · M6 failure detection +
repair · M7 payload WAL + crash recovery · M8 metadata service split ·
M9 cache + HTTP gateway · M10 observability · M11 Docker · M12 Kubernetes ·
M13 performance · M14 chaos · M15 (optional) Raft.
