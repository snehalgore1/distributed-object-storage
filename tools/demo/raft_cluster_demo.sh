#!/usr/bin/env bash
# Live demo of the Raft-replicated metadata control plane (spec Milestone 15).
#
# Boots three dos_metadata processes as a Raft group, shows which one is elected
# leader, kills it, and shows a survivor win the next election — the acceptance
# scenario, end to end, over real gRPC.
#
# Usage: tools/demo/raft_cluster_demo.sh [path-to-dos_metadata]
set -euo pipefail

BIN="${1:-build/dos_metadata}"
if [[ ! -x "$BIN" ]]; then
  echo "dos_metadata not found at '$BIN'. Build it first:"
  echo "  cmake --build build --target dos_metadata"
  echo "Then: $0 <path-to-dos_metadata>"
  exit 1
fi

WORK="$(mktemp -d)"
PIDS=()
cleanup() {
  echo
  echo "== cleaning up =="
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

# Metadata service ports :9000-9002, Raft peer ports :9100-9102.
# (Plain functions, not associative arrays, so this runs on macOS bash 3.2.)
meta_port() { case "$1" in A) echo 9000;; B) echo 9001;; C) echo 9002;; esac; }
raft_port() { case "$1" in A) echo 9100;; B) echo 9101;; C) echo 9102;; esac; }

start_node() {
  local id="$1"
  local peers=()
  for other in A B C; do
    [[ "$other" == "$id" ]] && continue
    peers+=( --raft-peer "${other}=127.0.0.1:$(raft_port "$other")" )
  done
  "$BIN" --id "$id" \
    --address "0.0.0.0:$(meta_port "$id")" \
    --raft-address "0.0.0.0:$(raft_port "$id")" \
    --raft-dir "$WORK/raft-$id" \
    "${peers[@]}" \
    >"$WORK/$id.log" 2>&1 &
  PIDS+=( $! )
  echo "started node $id (metadata :$(meta_port "$id"), raft :$(raft_port "$id"), pid $!)"
}

latest_role_is_leader() {
  # Succeeds if node $1's most recent role event is 'leader elected'.
  local id="$1"
  [[ -f "$WORK/$id.log" ]] || return 1
  local last
  last="$(grep -E 'raft_leader_elected|raft_step_down' "$WORK/$id.log" 2>/dev/null | tail -1)"
  [[ "$last" == *raft_leader_elected* ]]
}

find_leader() {
  # Prints the id of a node whose latest role event is 'leader elected'.
  for id in A B C; do
    if latest_role_is_leader "$id"; then
      echo "$id"
      return 0
    fi
  done
  return 1
}

echo "== starting 3-node Raft metadata cluster =="
for id in A B C; do start_node "$id"; done

echo
echo "== waiting for leader election =="
leader=""
for _ in $(seq 1 50); do
  leader="$(find_leader || true)"
  [[ -n "$leader" ]] && break
  sleep 0.2
done

if [[ -z "$leader" ]]; then
  echo "no leader elected; logs:"
  for id in A B C; do echo "--- $id ---"; cat "$WORK/$id.log"; done
  exit 1
fi
echo "LEADER ELECTED: node $leader"
grep raft_leader_elected "$WORK/$leader.log" | tail -1

echo
echo "== killing leader $leader =="
# Find and kill the leader's process.
for i in "${!PIDS[@]}"; do
  node_order=(A B C)
  if [[ "${node_order[$i]}" == "$leader" ]]; then
    kill "${PIDS[$i]}" 2>/dev/null || true
  fi
done

echo "== waiting for a survivor to win the next election =="
new_leader=""
for _ in $(seq 1 50); do
  for id in A B C; do
    [[ "$id" == "$leader" ]] && continue
    # Count leader-elected events after the kill: the last line wins.
    if grep -q raft_leader_elected "$WORK/$id.log" 2>/dev/null; then
      # Ensure this node's latest role event is leader (not a stale earlier one).
      last="$(grep -E 'raft_leader_elected|raft_step_down' "$WORK/$id.log" | tail -1)"
      if [[ "$last" == *raft_leader_elected* ]]; then
        new_leader="$id"
      fi
    fi
  done
  [[ -n "$new_leader" && "$new_leader" != "$leader" ]] && break
  sleep 0.2
done

if [[ -z "$new_leader" ]]; then
  echo "no new leader after failover; survivor logs:"
  for id in A B C; do [[ "$id" == "$leader" ]] && continue; echo "--- $id ---"; cat "$WORK/$id.log"; done
  exit 1
fi

echo "NEW LEADER AFTER FAILOVER: node $new_leader"
grep -E 'raft_election_started|raft_leader_elected' "$WORK/$new_leader.log" | tail -3
echo
echo "== SUCCESS: leadership survived the failure of node $leader =="
