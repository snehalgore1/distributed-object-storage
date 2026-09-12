# Raft Metadata Coordination

> Status: Milestone 15 (optional tier). The control plane runs as a
> Raft-replicated group so a metadata leader failure never loses committed
> metadata and writes continue after a new leader is elected.

## Why

Through Milestone 8 the control plane — cluster membership, consistent-hash
placement, and the object→replica map — lived in a single `Metadata` process.
Correct and simple, but a single point of failure: if it died, no one could
look up where an object lived or record a new write. Milestone 15 replicates
that state across an odd-sized group (three nodes) with Raft, keeping every
copy identical and surviving the loss of a minority.

Only *metadata* goes through consensus. Object payloads never enter the Raft
log — they move on the data plane as before. The log carries just the mutations
(`RegisterObject`, `RemoveObject`, `AddNode`).

## The pieces

| Component | Role |
|-----------|------|
| [`RaftNode`](../include/consensus/raft_node.h) | The algorithm: terms, elections, heartbeats, `AppendEntries`/`RequestVote`, majority commit, follower catch-up, leader failover, deterministic apply. |
| [`RaftStorage`](../include/consensus/raft_storage.h) | Crash-safe durable state: `currentTerm`, `votedFor`, and the log. CRC-framed, `fsync`'d, torn-tail-tolerant (same discipline as the [WAL](storage-engine.md)). |
| [`RaftTransport`](../include/consensus/raft_transport.h) | Outbound peer RPCs, abstracted. Two implementations: an in-memory transport for deterministic tests, and a [gRPC transport](../include/network/raft_client.h) + [server](../include/network/raft_server.h) for a real multi-process cluster. |
| [`RaftMetadataRepository`](../include/cluster/raft_metadata_repository.h) | Binds Raft to the state machine: mutations are proposed to the log; committed entries are applied deterministically to a plain [`MetadataRepository`](../include/cluster/metadata_repository.h). Reads are served from applied state. |

Because the same committed command sequence is applied in the same order on
every node, each replica's `MetadataRepository` is byte-for-byte identical — so
a recovered or brand-new follower rebuilds exact state just by replaying the
log.

## How a write commits

```
client → leader.RegisterObject(loc)
  1. leader appends {term, index, command} to its log and fsyncs it
  2. leader replicates via AppendEntries to followers (in parallel)
  3. once a MAJORITY (2 of 3) have the entry, it is COMMITTED
  4. leader applies it to the MetadataRepository and returns OK
  5. followers apply it as they learn the advanced commit index
```

A write is acknowledged only after step 3. Committing on a majority is what
guarantees durability across failover: any node that can win the next election
must already hold every committed entry (the election "up-to-date log"
restriction, Raft §5.4.1), so a committed mutation can never be lost.

## What survives what

| Failure | Behavior |
|---------|----------|
| Leader crashes | A follower's election timeout fires, it wins a majority vote, and becomes leader. Writes resume against it. Committed entries are intact. |
| Minority (1 of 3) down or partitioned | The majority keeps electing/committing normally. The isolated node cannot win an election (no majority) and cannot commit. |
| No majority reachable | No leader; `Propose` returns `UNAVAILABLE`. The system refuses to acknowledge a write it cannot make durable rather than losing data. |
| Node rejoins / new node added | The leader detects its log is short (via the `AppendEntries` consistency check) and backs up `nextIndex` until it finds the match point, then ships the missing suffix — the node catches up. |
| Whole cluster restarts | Each node reloads `currentTerm`, `votedFor`, and its log from disk, re-elects, and re-applies committed entries. Nothing committed is lost. |

## Run a live cluster

Three metadata nodes, each with a distinct `--id` and the other two as
`--raft-peer`:

```sh
dos_metadata --id A --address 0.0.0.0:9000 --raft-address 0.0.0.0:9100 \
  --raft-dir /var/lib/dos/raft-A --raft-peer B=hostB:9101 --raft-peer C=hostC:9102
# ...and B, C symmetrically.
```

The scripted demo boots three local nodes, shows the elected leader, kills it,
and shows a survivor win the next election:

```sh
cmake --build build --target dos_metadata
tools/demo/raft_cluster_demo.sh build/dos_metadata
```

Role transitions are logged as structured JSON
(`raft_election_started`, `raft_leader_elected`, `raft_step_down`).

## Tests

- [`tests/unit/raft_storage_test.cc`](../tests/unit/raft_storage_test.cc) —
  durable state survives reopen; a torn tail from a crash mid-append is dropped,
  not mis-decoded.
- [`tests/integration/raft_test.cc`](../tests/integration/raft_test.cc) — a
  three-node cluster over the in-memory transport (real threads, deterministic
  failure injection): single-leader election, replication to all nodes, and the
  spec's acceptance scenario — **kill the leader → a survivor is elected →
  writes continue → a recovered node catches up → no committed mutation is
  lost** — plus crash recovery purely from the on-disk log.

The integration tests run in the ThreadSanitizer CI job (they use the in-memory
transport, so they are TSan-clean unlike the gRPC-backed suites).

## Scope and simplifications

- **No log compaction / snapshots.** The log grows unbounded. At metadata scale
  and portfolio scope this is fine; snapshotting is the natural next extension.
- **Static membership.** Bootstrap membership comes from identical config on
  every replica; dynamic `AddNode` goes through the log. Full Raft joint-consensus
  membership changes are not implemented.
- **Log conflict resolution rewrites the log file.** Correct and simple;
  truncations are rare (only on divergence during leader changes).
