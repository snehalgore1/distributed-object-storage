# Distributed Object Store

A fault-tolerant distributed object storage system in C++20 — built systems-first
to demonstrate concurrency, storage durability, distributed coordination, and
performance engineering.

> **Status:** Milestones 0–3 complete. A correct, tested, **concurrent**
> single-node storage engine (checksums, atomic durable writes, versioning,
> SQLite metadata, bounded-queue thread pool, sharded locking — TSan-clean) plus
> the **placement** layer: a consistent-hash ring with virtual nodes and a
> cluster map. Remaining distributed layers (gRPC transport, quorum replication,
> failure detection, WAL recovery) are on the roadmap below.

## What works today

- **`ObjectStore`** contract: `PUT` / `GET` / `HEAD` / `DELETE` / `LIST`.
- **`LocalObjectStore`**: filesystem-backed store with an atomic, fsync'd write
  path — a partially written object is never observable as committed.
- **Integrity**: every object carries a SHA-256 checksum, verified on read;
  corruption is detected, not served.
- **Versioning**: monotonic per-key versions; delete is a tombstone.
- **Metadata**: SQLite (`objects` table) behind a `MetadataStore` interface, the
  seam for the future control/data-plane split.
- **Concurrency**: fixed-size `ThreadPool` with a bounded queue (overload →
  `kUnavailable`, no unbounded growth) and **sharded locking** on the object
  store (exclusive for writes, shared for reads). ThreadSanitizer-clean.
- **Placement**: 64-bit **consistent-hash ring** with virtual nodes and a
  `ClusterMap`; deterministic key→node mapping and replica selection. Adding a
  node moves only ~1/N of keys (measured: 25% vs modulo's 75% going 3→4 nodes).

## Build & test

Requires a C++20 compiler, CMake ≥ 3.20, Ninja, GoogleTest, and SQLite3.

```sh
# macOS deps
brew install cmake ninja googletest sqlite

cmake -S . -B build -G Ninja -DDOS_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run under a sanitizer (ThreadSanitizer shown; also `address`, `undefined`):

```sh
cmake -S . -B build-tsan -G Ninja -DDOS_SANITIZER=thread
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

### Try it

```sh
./build/storage_demo /tmp/dos-demo
```

```
PUT   key=greeting/hello.txt version=1 size=24 sha256=9e3697...
GET   24 bytes: "hello, distributed world"
      byte-identical? yes
HEAD  version=1 deleted=0
LIST  1 object(s)
DELETE then GET -> NOT_FOUND: tombstoned: greeting/hello.txt (expected NOT_FOUND)
OK
```

Consistent-hashing rebalancing benchmark (modulo vs. consistent, 3→4 nodes):

```sh
./build/rebalance_bench            # ./build/rebalance_bench [num_keys] [vnodes]
```

```
Distribution across 3 nodes:  node-a 32.30%  node-b 33.46%  node-c 34.24%
Adding a 4th node moves:      consistent 25.25%    modulo 74.99%   (~3x fewer)
```

## Layout

```
include/  common/ (status, sha256, digest)  storage/ (interfaces + impls)
src/      implementations
tests/    unit/ (GoogleTest)
tools/    demo/ (storage_demo smoke tool)
docs/     architecture.md, storage-engine.md
```

## Documentation

- [Architecture](docs/architecture.md) — components and target topology.
- [Storage engine](docs/storage-engine.md) — durability contract and exact
  crash behavior at each PUT boundary.

## Roadmap

| Milestone | Focus |
|-----------|-------|
| ✅ M0 | Build/test foundation (CMake, CI, GoogleTest, clang-format) |
| ✅ M1 | Single-node storage engine (checksums, atomic writes, versioning) |
| ✅ M2 | Concurrency: thread pool, bounded queue, sharded locks (TSan-clean) |
| ✅ M3 | Consistent-hash ring + virtual nodes + cluster map (placement) |
| M4 | Replication factor 3 + quorum writes |
| M5 | Versioning & idempotency (conditional PUT, request IDs) |
| M6 | Failure detection + replica repair |
| M7 | Write-ahead log + crash recovery |
| M8 | Metadata service / control-plane split |
| M9 | LRU cache + HTTP gateway + CLI |
| M10 | Observability (Prometheus + Grafana, structured logs) |
| M11 | Docker Compose |
| M12 | Kubernetes (kind) |
| M13 | Performance engineering + benchmarks |
| M14 | Failure & chaos testing |
| M15 | (Optional) Raft metadata coordination |

## License

MIT — see [LICENSE](LICENSE).
