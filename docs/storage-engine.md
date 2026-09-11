# Storage Engine (Milestone 1)

The single-node engine (`LocalObjectStore`) stores object bytes on the local
filesystem and their metadata in SQLite. It is the durable substrate the
distributed layers (replication, quorum, repair) are built on.

## On-disk layout

```
<root>/
  data/
    tmp/                     staging area for in-flight writes
    <aa>/<bb>/<key-digest>   committed object payloads (2-level fan-out)
  metadata/
    objects.sqlite           committed metadata — the source of truth
```

- The **physical path** is derived from `sha256(key)` (see `common/digest`), not
  from the key text. The logical key lives only in metadata, never as a
  filename. This keeps any single directory from growing without bound and
  avoids filesystem-unsafe key characters.
- A filename is **never** treated as proof that an object is valid. An object is
  committed only when its metadata row exists.

## Metadata schema (spec §15.1)

```sql
CREATE TABLE objects (
  key        TEXT PRIMARY KEY,
  size       BIGINT  NOT NULL,
  checksum   TEXT    NOT NULL,   -- sha256 hex of payload
  version    BIGINT  NOT NULL,   -- monotonic per key
  created_at BIGINT  NOT NULL,   -- unix epoch seconds
  deleted    INTEGER NOT NULL DEFAULT 0
);
```

SQLite runs in WAL journal mode so metadata commits are crash-safe.

## Durability contract (the PUT write path)

`LocalObjectStore::Put` performs, in order:

1. Reject empty keys.
2. Compute `sha256(payload)` and assign the version.
3. **Append a WAL `BEGIN` record** (seq, op, key, version, checksum, size) and
   `fsync` it — the intent is durable before any visible change.
4. Write the payload to a unique file in `data/tmp/`.
5. `fsync` the temp file.
6. Atomically `rename` the temp file to its final `<aa>/<bb>/<digest>` path.
7. `fsync` the parent directory so the rename itself is durable.
8. Commit the metadata row (SQLite, WAL-journaled) — the visibility point.
9. **Append a WAL `COMMIT` record** marking the operation complete.

**Crash behavior at each boundary** — what an interviewer can ask about:

| Crash point                          | Result                                                                              |
|--------------------------------------|-------------------------------------------------------------------------------------|
| Before the WAL `BEGIN` (step 3)      | Nothing happened; object never existed. Safe.                                        |
| After `BEGIN`, before rename (6)     | Recovery sees `BEGIN` with no committed metadata and no intact payload → **discards** it. Safe. |
| After rename, before metadata (8)    | Recovery sees `BEGIN` + durable payload whose checksum matches → **completes the commit**. Safe. |
| After metadata commit (8)            | Fully committed; the (possibly missing) `COMMIT` record is irrelevant — recovery sees the effect is already present. Survives restart. |

The key invariant: **a partially written object is never observable as a
committed version.** The metadata commit is the single point that makes a
version visible; the WAL makes the crash window between "bytes durable" and
"metadata committed" explicitly recoverable.

## Write-ahead log & crash recovery (Milestone 7)

The WAL (`wal/node.wal`, see [`Wal`](../include/storage/wal.h)) is an append-only
log of length-prefixed, **CRC32-checked** records, `fsync`'d on every append. A
crash mid-append leaves a torn tail, which replay detects (bad length or CRC) and
ignores.

On `LocalObjectStore::Open`, recovery:

1. Replays the WAL and notes which sequences reached `COMMIT`.
2. For each `BEGIN` **without** a `COMMIT` (an interrupted operation):
   - **PUT** — if the metadata is already at/after this version, it's done; else
     if the payload is on disk and its **checksum matches** the WAL record,
     complete the commit by writing metadata; otherwise discard it.
   - **DELETE** — apply the tombstone if the key still exists.
3. Removes orphaned files from `data/tmp/`.
4. **Checkpoints** by truncating the WAL — metadata is now the source of truth.

Recovery is **idempotent**: it only ever writes the exact committed metadata a
completed operation would have, so running it twice yields the same state. A
crash after local commit but before replication leaves a fully committed local
object that the replica-repair path (Milestone 6) reconciles across the cluster.

## Integrity on read

`Get` reads the payload and recomputes its SHA-256, comparing against the
committed checksum. A mismatch returns `kChecksumMismatch` rather than serving
corrupt bytes — this is what later milestones use to fail over to another
replica.

## Delete semantics

`Delete` sets a tombstone (`deleted = 1`). Tombstoned keys report `kNotFound`
from `Get`/`Head` and are excluded from `List`. Physical payload cleanup and
version-conditional writes are deferred to later milestones (M5).

## Concurrency (Milestone 2)

`LocalObjectStore` is safe for concurrent use. Instead of one global mutex, keys
are hashed to a fixed set of **striped locks** (`kNumShards` `std::shared_mutex`,
indexed by `hash(key) % kNumShards`):

- `Put` / `Delete` take the key's shard **exclusively**.
- `Get` / `Head` take it **shared** (many concurrent readers).

Operations on keys in different shards proceed in parallel. The exclusive lock in
`Put` is what makes the *read-current-version → write-version+1* sequence atomic,
so concurrent writers to the same key produce a clean monotonic version sequence
rather than racing. The SQLite connection is opened with `SQLITE_OPEN_FULLMUTEX`
(serialized mode) so the shared handle is safe across threads.

Work is executed on a fixed-size [`ThreadPool`](../include/common/thread_pool.h)
with a **bounded queue**: when the queue is full, `Submit` returns
`kUnavailable` (backpressure) rather than growing without limit. A task that
throws is caught and counted; it never tears down its worker.

All concurrency tests run clean under **ThreadSanitizer** (`-DDOS_SANITIZER=thread`).

## Known limitations (addressed later)

- No WAL for the object payloads yet (spec M7); durability rests on the
  fsync/rename ordering above plus SQLite's WAL for metadata.
- Overwrites reuse the same physical path (keyed by digest); only the latest
  committed version is retained.
