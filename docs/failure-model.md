# Failure Model, Detection & Repair

> Status: Milestone 6. Failure detection (heartbeat state machine) and
> anti-entropy replica repair.

## What can fail, and what the system promises

| Failure | Behavior |
|---------|----------|
| One replica down | Reads still succeed from another replica (M4 fallback); writes still meet W=2 quorum. The downed replica is marked **lagging**. |
| Replica returns corrupt bytes | The node's own read verifies its checksum and returns `CHECKSUM_MISMATCH`; the coordinator falls back to a healthy replica. Corruption is never served or propagated. |
| Two replicas down (of 3) | Writes fail cleanly with `UNAVAILABLE` (quorum unreachable) rather than acknowledging a non-durable write. |
| Node rejoins after downtime | It is repaired from healthy peers back to full redundancy before being trusted. |

## Failure detection

[`FailureDetector`](../include/cluster/failure_detector.h) tracks each node with
a heartbeat state machine:

```
Healthy --miss--> Suspect --miss (reaches threshold)--> Unavailable
   ^                 |                                       |
   |              success                                 success
   |                 v                                       v
   +-------------- Healthy                              Recovering --repair done--> Healthy
```

- **Miss threshold** (default 3): a single transient timeout moves a node to
  `Suspect`, not `Unavailable`. Only sustained misses declare it dead. This is
  the "don't react to every transient network delay" requirement.
- **Recovering, not Healthy, on return**: a node that comes back after being
  `Unavailable` enters `Recovering`. Successful heartbeats alone do **not**
  re-trust it — it is promoted to `Healthy` only after repair completes. This
  avoids serving stale data from a node that missed writes while gone.
- **Liveness ≠ TCP**: the `Health` RPC is answered only if the node's store
  actually responds (it lists objects), so a hung store is detected even while
  the socket is open.

The detector is driven by heartbeat outcomes (`RecordSuccess`/`RecordFailure`),
which keeps the state machine deterministic and unit-testable independent of
timers and sockets. Suggested parameters (spec §13.1): heartbeat interval 1 s,
miss threshold 3, RPC timeout 200–500 ms.

## Anti-entropy repair

[`Repairer`](../include/network/repairer.h) reconciles a target node against its
healthy peers:

1. **Survey peers**: `List` every peer and build the highest-known version of
   each object, remembering which peer to pull it from.
2. **Scope to responsibility**: consider only objects whose consistent-hash
   placement includes the target node.
3. **Copy what's missing or stale**: if the target lacks the object or holds an
   older version, pull the bytes from a peer, **re-verify the SHA-256 checksum**,
   and write it at the peer's version.
4. **Never propagate corruption**: peers verify integrity on read, and the
   repairer verifies again before writing, so a corrupt source is skipped
   (counted as failed) rather than copied.

The result is a report of `{copied, already_current, failed}`. In production this
runs on a separate/rate-limited worker so repair traffic does not starve client
traffic (spec pitfall); the mechanism is a plain call today and slots onto the
existing `ThreadPool`.

## Tested guarantees

- `FailureDetector` FSM: transient miss → Suspect; threshold → Unavailable;
  return → Recovering (not Healthy) until repair; re-death → Unavailable
  (`tests/unit/failure_detector_test.cc`).
- Repair end-to-end (`tests/integration/replication_test.cc`): a node misses
  writes while down, rejoins, and repair restores every object it is responsible
  for — checksum-valid — bringing it back to full redundancy.
