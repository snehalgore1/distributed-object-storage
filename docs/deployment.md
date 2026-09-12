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

## Kubernetes (kind) — Milestone 12

Manifests live in [`deploy/kubernetes/`](../deploy/kubernetes). They deploy:

- **Storage nodes** as a **StatefulSet** (`dos-node`, 3 replicas) — stable
  identities `dos-node-0/1/2` via a headless Service, and a per-pod
  **PersistentVolumeClaim** (`volumeClaimTemplates`) so object data is durable
  across pod restarts. The node id is the pod name.
- **Metadata** as a Deployment + Service, seeded with the node list from a
  **ConfigMap**.
- **Gateway** as a Deployment (2 replicas — scales independently of storage) +
  a **NodePort** Service, reading its config from the ConfigMap and a
  (placeholder) **Secret** env var.
- **Liveness/readiness probes** on every component (TCP for the gRPC services,
  HTTP `/metrics` for the gateway); readiness gates traffic so a Service never
  routes to a pod that isn't ready.

```sh
kind create cluster --name dos --config deploy/kubernetes/kind-config.yaml
kind load docker-image dos:latest --name dos      # after: docker build -f deploy/docker/Dockerfile -t dos:latest .
kubectl apply -f deploy/kubernetes/
kubectl -n dos rollout status statefulset/dos-node

curl -X PUT --data-binary "hi" http://localhost:30080/objects/greeting
curl http://localhost:30080/objects/greeting
```

**Pod restart preserves state** (verified): deleting a storage pod lets the
StatefulSet recreate it, reattaching the *same* PVC — its `/data` (object files
+ SQLite metadata) survives and the object is still served:

```sh
kubectl -n dos delete pod dos-node-0
kubectl -n dos wait --for=condition=Ready pod/dos-node-0
curl http://localhost:30080/objects/greeting   # still returns "hi"
```

Tear down: `kind delete cluster --name dos`.

## Configuration

Services are configured by their `command:` flags in the compose file
(`--address`, `--data-dir`, `--metadata`, and the `--node id=host:port` list the
metadata service is seeded with). Prometheus reads
[`deploy/prometheus/prometheus.yml`](../deploy/prometheus/prometheus.yml);
Grafana is provisioned from [`deploy/grafana/provisioning`](../deploy/grafana/provisioning)
with the [dashboard](../deploy/grafana/dashboards/dos-overview.json).
