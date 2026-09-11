# Observability & Debugging

> Status: Milestone 10 — structured logs, request IDs, Prometheus metrics, and a
> Grafana dashboard.

## Request IDs & structured logs

Every request entering the gateway gets a **request id** (`NewRequestId()`),
returned to the client as the `X-Request-Id` header and emitted in a structured
JSON log line ([`Logger`](../include/common/logging.h)):

```json
{"ts_ms":1789156000158,"level":"info","msg":"http_request","request_id":"cf2589127bf858c6","method":"GET","path":"/objects/k1","status":"200","duration_ms":"0.872375"}
```

**Trace one request end to end:** take the `X-Request-Id` from a client response
(or a slow log line) and grep the logs for it:

```sh
grep '"request_id":"cf2589127bf858c6"' gateway.log
```

Logs are one JSON object per line, so they pipe cleanly into `jq` or a log
aggregator. (Propagating the id further into the storage/metadata RPCs — via the
existing `request_id` field and gRPC metadata — is a natural extension.)

## Metrics

The gateway serves the Prometheus text exposition at **`GET /metrics`** — no
code changes or restarts needed to scrape it ([`MetricsRegistry`](../include/common/metrics.h)
implements counters, gauges, and histograms).

| Metric | Type | Meaning |
|--------|------|---------|
| `dos_http_requests_total{method,code}` | counter | traffic volume / status mix |
| `dos_http_errors_total{method}` | counter | 5xx error rate |
| `dos_http_request_duration_seconds` | histogram | p50/p95/p99 latency (via `histogram_quantile`) |
| `dos_http_in_flight` | gauge | concurrency / saturation |
| `dos_cache_hits` / `dos_cache_misses` / `dos_cache_hit_ratio` | gauge | metadata cache effectiveness |
| `dos_replicas{state}` | gauge | replica health (healthy / lagging / failed) |

Example (`curl localhost:8080/metrics`):

```
# TYPE dos_http_requests_total counter
dos_http_requests_total{code="200",method="GET"} 2
dos_http_requests_total{code="201",method="PUT"} 1
dos_http_request_duration_seconds_bucket{method="GET",le="0.005"} 2
...
dos_cache_hit_ratio 0.666667
dos_replicas{state="healthy"} 3
dos_replicas{state="lagging"} 0
```

A node failure shows up here as `dos_replicas{state="lagging"}` (or `failed`)
rising — visible in both the metrics and the logs.

## Prometheus + Grafana

- [`deploy/prometheus/prometheus.yml`](../deploy/prometheus/prometheus.yml) —
  scrapes the gateway's `/metrics`.
- [`deploy/grafana/dashboards/dos-overview.json`](../deploy/grafana/dashboards/dos-overview.json)
  — a dashboard with request rate, p50/p95/p99 latency, error rate, cache hit
  ratio, replica state, and in-flight requests.

These are wired into the one-command stack in Milestone 11 (Docker Compose),
where Prometheus and Grafana run alongside the cluster.
