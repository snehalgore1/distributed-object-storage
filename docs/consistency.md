# Placement and Consistency

> Status: Milestone 3 implements the **placement** layer (consistent-hash ring +
> cluster map). Replication and quorum semantics (the consistency guarantees
> themselves) arrive in Milestone 4.

## Why consistent hashing (not `hash(key) % N`)

Modulo placement ties every key's location to the node *count*. Growing from 3
to 4 nodes changes `% 3` to `% 4` for almost every key, so ~75% of the keyspace
has to move — a data-movement storm on every membership change.

Consistent hashing places both nodes and keys on a 64-bit ring. A key is owned
by the first node encountered clockwise. Adding a node only steals the arc of
the ring immediately behind its new positions, so only ~`1/N` of keys move, and
they move **only to the new node** — existing nodes never exchange keys with
each other.

Measured (100k keys, 200 virtual nodes, `tools/clusterbench/rebalance_bench`):

```
Distribution across 3 nodes:  node-a 32.30%   node-b 33.46%   node-c 34.24%
Adding a 4th node moves:      consistent 25.25%    modulo 74.99%   (~3x fewer)
```

## Virtual nodes

Placing each physical node at a single ring position gives lumpy balance and a
big single arc to hand off. Instead each physical node owns `vnodes` positions
(hash of `"<node_id>#<i>"`), so its share of the keyspace is the sum of many
small arcs. This tightens distribution (above) and spreads the keys that move on
a membership change across the whole ring. Default: 150–200 virtual nodes.

## Hashing

Ring positions use [`Hash64`](../include/common/hash.h) — the top 8 bytes of
SHA-256, big-endian. It is deliberately **not** `std::hash`: placement must be
identical across machines, rebuilds, and standard-library versions, or the same
key would resolve to different nodes on different hosts.

## Placement API

- [`ConsistentHashRing`](../include/cluster/consistent_hash_ring.h) —
  `LookupPrimary(key)` and `LookupReplicas(key, count)` (primary first, then
  successive **distinct** physical nodes clockwise).
- [`ClusterMap`](../include/cluster/cluster_map.h) — membership table of
  [`NodeInfo`](../include/cluster/node_info.h) plus the ring; `PlacementFor(key,
  rf)` returns the replica set of size `min(rf, cluster size)`.

The replica set produced here is exactly what Milestone 4 writes to under a
quorum, and what Milestone 6 repairs when a node fails.

## Not yet decided here

Consistency (what a read guarantees after a write) is a property of the
replication protocol, not placement. That is defined and tested in Milestone 4
(RF=3, write quorum W=2). Until then, placement simply answers *where* a key's
replicas should live.
