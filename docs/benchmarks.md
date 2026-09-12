# Benchmarks & Performance

> Status: Milestone 13. Controlled experiments with a concurrent load generator.
> Reproduced on an **Apple M1 Pro (8 cores), 16 GB, macOS 26**, all services on
> one host over loopback. Numbers characterize *this* setup — rerun locally
> rather than quoting them verbatim (the evidence rule).

Tools:
- [`loadgen`](../tools/bench/loadgen.cc) — multi-threaded HTTP load generator
  (`--threads`, `--duration`, `--size`, `--read-ratio`, `--keyspace`), reports
  throughput and p50/p95/p99.
- [`micro_bench`](../tools/bench/micro_bench.cc) — single-node engine PUT/GET.
- [`rebalance_bench`](../tools/clusterbench/rebalance_bench.cc) — consistent-hash
  key movement.

The gateway exposes knobs so each comparison changes exactly one variable:
`--rf` (replication factor → majority quorum) and `--no-cache`.

## Experiment A — concurrency scaling

RF=3, cache on, 4 KB objects, 80% reads; increasing client threads.

| Clients | Throughput (req/s) | p50 | p95 | p99 |
|--------:|-------------------:|----:|----:|----:|
| 1  | 1,485 | 0.42 ms | 1.58 ms | 2.19 ms |
| 8  | 3,809 | 1.65 ms | 3.86 ms | 4.95 ms |
| 32 | 3,861 | 7.78 ms | 10.69 ms | 20.62 ms |
| 64 | 3,850 | 15.79 ms | 20.37 ms | 53.30 ms |

**Reading it:** throughput scales ~2.6× from 1→8 clients, then **plateaus around
3,850 req/s** while latency keeps climbing (p50 0.4 → 15.8 ms). Past ~8 clients,
extra load just queues — the system is saturated, not faster.

## Experiment B — replication cost (RF=1 vs RF=3)

Writes only, 16 clients, 4 KB.

| Config | Throughput (req/s) | p50 | p95 |
|--------|-------------------:|----:|----:|
| RF=1 (W=1) | 2,490 | 5.98 ms | 8.47 ms |
| RF=3 (W=2) | 1,272 | 11.71 ms | 17.67 ms |

**Reading it:** RF=3 halves write throughput and roughly doubles latency versus
RF=1. That is the cost of durability/availability: every write fans out to three
replicas and the coordinator waits for a **2-of-3 quorum** (each replica does its
own fsync'd, WAL-logged commit) before acknowledging.

## Experiment C — metadata cache (on vs off)

Reads only, 16 clients, hot keyspace of 50 keys.

| Config | Throughput (req/s) | p50 | p95 |
|--------|-------------------:|----:|----:|
| cache on  | 5,914 | 2.63 ms | 2.88 ms |
| cache off | 4,563 | 3.42 ms | 4.31 ms |

**Reading it:** the LRU metadata cache lifts read throughput ~30% and cuts p50
latency ~23% on a hot keyspace by eliminating a metadata-service round trip per
read. The win grows with read skew and with metadata-service latency.

## Bottleneck identified

Two, with evidence:

1. **Throughput saturates near 8 concurrent clients** (Experiment A). All
   services share one 8-core host, and the write path is dominated by the
   durability sequence (WAL fsync + object fsync + directory fsync per replica).
   Adding clients past saturation only grows queueing latency. Next steps:
   group/batch commit, or relax the fsync boundary behind an explicit durability
   setting.
2. **Large-object GET is CPU-bound on checksum verification** (from
   `micro_bench`): every read recomputes SHA-256 over the whole payload, and the
   hash is a portable, unoptimized reference implementation — for 1 MB objects,
   hashing (not I/O) dominates. Next step: a hardware-accelerated SHA-256.

## Reproduce

Start a local cluster (metadata + 3 nodes + gateway), then:

```sh
# concurrency scaling
./build/loadgen --gateway 127.0.0.1:8080 --threads 8  --duration 6 --size 4096 --read-ratio 0.8

# replication cost: run the gateway with --rf 1 vs --rf 3, writes only
./build/loadgen --gateway 127.0.0.1:8080 --threads 16 --duration 6 --read-ratio 0.0

# cache effect: run the gateway with and without --no-cache, reads on a hot keyspace
./build/loadgen --gateway 127.0.0.1:8080 --threads 16 --duration 6 --read-ratio 1.0 --keyspace 50
```
