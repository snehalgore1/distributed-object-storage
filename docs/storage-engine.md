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
2. Compute `sha256(payload)` and assign `version = current + 1`.
3. Write the payload to a unique file in `data/tmp/`.
4. `fsync` the temp file.
5. Atomically `rename` the temp file to its final `<aa>/<bb>/<digest>` path.
6. `fsync` the parent directory so the rename itself is durable.
7. Commit the metadata row (WAL).

**Crash behavior at each boundary** — what an interviewer can ask about:

| Crash point                        | Result                                                        |
|------------------------------------|---------------------------------------------------------------|
| Before step 5 (rename)             | Only an orphan temp file exists; object is invisible. Safe.   |
| After rename, before metadata (7)  | Payload exists on disk but no committed metadata → invisible; overwritten by the next successful PUT to the key. Safe. |
| After metadata commit              | Fully committed; survives restart.                            |

The key invariant: **a partially written object is never observable as a
committed version.** The metadata commit is the single point that makes a
version visible.

## Integrity on read

`Get` reads the payload and recomputes its SHA-256, comparing against the
committed checksum. A mismatch returns `kChecksumMismatch` rather than serving
corrupt bytes — this is what later milestones use to fail over to another
replica.

## Delete semantics

`Delete` sets a tombstone (`deleted = 1`). Tombstoned keys report `kNotFound`
from `Get`/`Head` and are excluded from `List`. Physical payload cleanup and
version-conditional writes are deferred to later milestones (M5).

## Known limitations (addressed later)

- No WAL for the object payloads yet (spec M7); durability rests on the
  fsync/rename ordering above plus SQLite's WAL for metadata.
- No concurrency control yet (spec M2): a single `LocalObjectStore` is not yet
  guarded for concurrent writers to the same key.
- Overwrites reuse the same physical path (keyed by digest); only the latest
  committed version is retained.
