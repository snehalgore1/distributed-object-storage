# Protocol and Replication

> Status: Milestone 4. Defines the gRPC data-plane contract and the coordinator's
> quorum write / fallback read protocol.

## Transport

Services are defined in [`proto/storage.proto`](../proto/storage.proto) and
carried over gRPC (insecure channels for local/dev; TLS is a later concern).

### `StorageNode` service (per-node data plane)

Each storage node process ([`dos_node`](../tools/node/node_main.cc)) serves its
local object store:

| RPC | Purpose |
|-----|---------|
| `Put(key, data, version)` | Write at the coordinator-assigned version; the node computes and stores the checksum. |
| `Get(key)` | Return bytes; the node verifies the stored checksum first (mismatch → `CHECKSUM_MISMATCH`). |
| `Head(key)` | Return metadata only. |
| `Delete(key)` | Tombstone the object. |

Application outcomes travel as a `Code` enum in the response (mirrors
`dos::StatusCode`); transport failures surface as the gRPC status. RPCs carry a
bounded client deadline so a dead node can't block a caller.

## Coordinator quorum protocol

The [`Coordinator`](../include/network/coordinator.h) uses the
[`ClusterMap`](../include/cluster/cluster_map.h) to place each key on **RF**
replicas and drives the read/write path. Defaults: **RF = 3, W = 2**.

### Write path

1. `replicas = PlacementFor(key, RF)` (primary + distinct successors on the ring).
2. Assign a **version**: `max(version seen on replicas) + 1` (the coordinator
   owns versioning so replicas agree; conditional/version-guarded writes are
   Milestone 5).
3. Fan out `Put(key, data, version)` to all replicas **in parallel**.
4. **ACK the client once W replicas succeed.** A write that reaches fewer than W
   replicas returns `UNAVAILABLE` — it is not reported as durable.
5. Replicas that didn't ack are marked **lagging** (repair candidates for
   Milestone 6); reachable failures are marked **failed**. Health is exposed via
   `ReplicaHealthSnapshot()`.

"Durable enough to ACK" is therefore defined precisely: **W = 2 of RF = 3
replicas have committed the write through their local durable path**
(temp → fsync → rename → fsync dir → metadata commit; see
[storage-engine.md](storage-engine.md)).

### Read path

1. `replicas = PlacementFor(key, RF)`.
2. Try replicas in order (primary first). Return the first checksum-valid
   response. Because each node verifies integrity, a successful `Get` is already
   checksum-valid.
3. On a replica's failure or `CHECKSUM_MISMATCH`, **fall through to the next
   replica**. Only if every replica fails does the read fail.

### Conditional writes & idempotency (Milestone 5)

Beyond the unconditional write above, the coordinator offers a **conditional,
idempotent** write (`PutConditional`) — the safe primitive for concurrent
writers and client retries:

- **`expected_version`** guards the write: it commits only if the current
  committed version equals `expected_version` (0 = "must not already exist"),
  producing `expected_version + 1`. A stale writer gets `CONFLICT`, deterministically.
- Because the new version is a pure function of `expected_version`, **every
  replica and every retry computes the same version** — no version drift.
- **`request_id`** makes retries idempotent: if the request that produced the
  current version carries the same id, the replica returns the existing metadata
  instead of writing again. A client that times out and retries the identical
  request never creates a second, contradictory version.

Each replica enforces the condition locally under its per-key shard lock, so two
concurrent writers with the same `expected_version` resolve to exactly one winner
and one `CONFLICT`. The coordinator aggregates: a quorum of acks → success; a
condition rejection that blocks the quorum → `CONFLICT`.

Unconditional `Put` is *not* idempotent (a retry creates a new version); use
`PutConditional` with a `request_id` when you need at-most-once semantics.

### Delete semantics

`Delete` is a **logical tombstone** (`deleted = 1`), replicated to a quorum.
Tombstoned keys report `NOT_FOUND` from `Get`/`Head` and are excluded from
`List`, but the row (version + last request id) is retained so a later
conditional write can reference the current version. Physical payload reclamation
is deferred (a later milestone).

### Consistency model (honest statement)

- A write is acknowledged only after **W = 2** replicas commit, so any two
  writes to the same key have overlapping replica sets and the later version
  wins on the replicas it reaches.
- Reads are **not** quorum reads (R = 1 with fallback): a read returns the first
  available checksum-valid replica. Immediately after a write that reached only
  W of RF replicas, a read served by the lagging replica can observe the older
  version until repair (Milestone 6) reconciles it. This is last-write-wins with
  read-repair to come — **not** linearizable, and the README/docs say so.
- Integrity is always enforced: corrupt bytes are never returned; the read fails
  over to a healthy replica instead.

## Tested guarantees (`tests/integration/replication_test.cc`)

- A quorum write replicates to all nodes and reads back byte-identical.
- A write **succeeds with one replica down** (2 of 3 ack).
- A write **fails `UNAVAILABLE` with two replicas down** (quorum not met).
- A read **falls back on checksum mismatch**: corrupting the primary's on-disk
  copy still yields correct bytes from another replica.
- Delete tombstones across the replica set.
