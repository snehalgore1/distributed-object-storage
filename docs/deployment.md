# Deployment (Docker Compose)

> Status: Milestone 11 — the whole system, plus Prometheus and Grafana, in one
> command.

## One-command start

```sh
docker compose -f deploy/compose/docker-compose.yml up --build
```

This builds the image once (multi-stage: an Ubuntu build stage, then a slim
runtime with only the shared libraries) and starts:

| Service | Purpose | Port |
|---------|---------|------|
| `node-a`, `node-b`, `node-c` | storage nodes (data plane), each with a durable volume | — |
| `metadata` | control plane (placement + object→replica map) | — |
| `gateway` | HTTP API | `8080` |
| `prometheus` | scrapes the gateway `/metrics` | `9090` |
| `grafana` | dashboards (anonymous viewer enabled) | `3000` |

Startup ordering is enforced with health checks and `depends_on:
condition: service_healthy` — nodes become healthy (TCP probe), then metadata,
then the gateway (HTTP `/metrics` probe), then the metrics stack.

## Use it

```sh
curl -X PUT --data-binary "hello" http://localhost:8080/objects/greeting
curl http://localhost:8080/objects/greeting
open http://localhost:3000/d/dos-overview   # Grafana dashboard
open http://localhost:9090/targets          # Prometheus scrape targets
```

## Durability across restarts

Each storage node writes to a named Docker volume (`node-a-data`, …), so an
intended container restart preserves committed objects:

```sh
docker compose -f deploy/compose/docker-compose.yml restart node-a
curl http://localhost:8080/objects/greeting   # still returns "hello"
```

## Clean reset

```sh
# stop, keeping data volumes
docker compose -f deploy/compose/docker-compose.yml down

# stop AND wipe all storage (fresh cluster next time)
docker compose -f deploy/compose/docker-compose.yml down -v
```

## Configuration

Services are configured by their `command:` flags in the compose file
(`--address`, `--data-dir`, `--metadata`, and the `--node id=host:port` list the
metadata service is seeded with). Prometheus reads
[`deploy/prometheus/prometheus.yml`](../deploy/prometheus/prometheus.yml);
Grafana is provisioned from [`deploy/grafana/provisioning`](../deploy/grafana/provisioning)
with the [dashboard](../deploy/grafana/dashboards/dos-overview.json).
