# HTTP Gateway, CLI & Cache

> Status: Milestone 9 — an HTTP front door, a CLI, and an LRU metadata cache.

## HTTP gateway

[`HttpGateway`](../include/network/http_gateway.h) is a **thin adapter**: it maps
HTTP verbs/paths to coordinator calls and `StatusCode`s to HTTP codes, and holds
no business logic of its own. It runs on a minimal, self-contained HTTP/1.1
server ([`HttpServer`](../include/network/http_server.h) — POSIX sockets, a
bounded `ThreadPool`, `Content-Length` bodies, `Connection: close`).

| Method & path | Action | Success | Notable errors |
|---------------|--------|---------|----------------|
| `PUT /objects/<key>` (body = payload) | quorum write | `201` + `X-Object-Version` | `503` quorum unmet |
| `GET /objects/<key>` | read (checksum-verified) | `200` + bytes | `404` not found |
| `HEAD /objects/<key>` | metadata only | `200` + `X-Object-*` headers | `404` |
| `DELETE /objects/<key>` | tombstone | `204` | `404` |
| `GET /objects[?prefix=..]` | list (JSON) | `200` | — |

Status mapping: `kNotFound`→404, `kConflict`→409, `kInvalidArgument`→400,
`kChecksumMismatch`→422, `kUnavailable`→503, `kIoError`→500.

Keys may contain `/` (e.g. `photos/2026/cat.jpg`); the gateway takes everything
after `/objects/` as the key.

## LRU metadata cache

[`CachingMetadataView`](../include/cluster/caching_metadata_view.h) wraps any
`MetadataView` with an O(1) [`LruCache`](../include/common/lru_cache.h)
(`unordered_map` + intrusive doubly linked list). It caches object-location
lookups and stays coherent with writes: `RegisterObject` refreshes the cached
entry and `RemoveObject` invalidates it, so a cached read never returns a stale
replica set or version. Hit rate is exposed via `hit_rate()`.

The gateway wraps its `RemoteMetadataView` in this cache, so repeated reads of a
hot key are served without a round trip to the metadata service.

## CLI

[`dos_cli`](../tools/cli/cli_main.cc) is a small HTTP client:

```sh
dos_cli --gateway 127.0.0.1:8080 put greeting/hello "hello from the CLI"
dos_cli --gateway 127.0.0.1:8080 get greeting/hello
dos_cli --gateway 127.0.0.1:8080 head greeting/hello
dos_cli --gateway 127.0.0.1:8080 list
dos_cli --gateway 127.0.0.1:8080 delete greeting/hello
```

Anything that speaks HTTP works too:

```sh
curl -X PUT --data-binary @photo.jpg http://127.0.0.1:8080/objects/photos/cat.jpg
curl http://127.0.0.1:8080/objects/photos/cat.jpg -o out.jpg
curl http://127.0.0.1:8080/objects            # JSON listing
```

## Running the full stack

```sh
# storage nodes (data plane)
dos_node --id node-a --address 127.0.0.1:9001 --data-dir /tmp/dos/a &
dos_node --id node-b --address 127.0.0.1:9002 --data-dir /tmp/dos/b &
dos_node --id node-c --address 127.0.0.1:9003 --data-dir /tmp/dos/c &

# metadata service (control plane), told about the nodes
dos_metadata --address 127.0.0.1:9000 \
  --node node-a=127.0.0.1:9001 --node node-b=127.0.0.1:9002 --node node-c=127.0.0.1:9003 &

# HTTP gateway (discovers nodes from the metadata service)
dos_gateway --address 127.0.0.1:8080 --metadata 127.0.0.1:9000 &
```
