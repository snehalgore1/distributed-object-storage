#!/usr/bin/env bash
# Live chaos runner (spec Milestone 14): generate traffic against the Docker
# Compose stack while repeatedly killing and restarting a storage node, and
# confirm the gateway keeps serving reads throughout.
#
# Prereq: the compose stack is up
#   docker compose -f deploy/compose/docker-compose.yml up -d
#
# Usage: tools/chaos/chaos.sh [gateway_url] [rounds]
set -uo pipefail

GATEWAY="${1:-http://localhost:8080}"
ROUNDS="${2:-3}"
COMPOSE="docker compose -f deploy/compose/docker-compose.yml"
VICTIM="node-c"

echo "seeding an object..."
curl -fsS -X PUT --data-binary "survive-the-chaos" "$GATEWAY/objects/chaos/key" >/dev/null

reads_ok=0; reads_bad=0
probe() {  # a read must return the correct bytes or we count it bad
  local body
  body=$(curl -fsS "$GATEWAY/objects/chaos/key" 2>/dev/null)
  if [ "$body" = "survive-the-chaos" ]; then reads_ok=$((reads_ok+1)); else reads_bad=$((reads_bad+1)); fi
}

for r in $(seq 1 "$ROUNDS"); do
  echo "=== round $r: killing $VICTIM ==="
  $COMPOSE kill "$VICTIM" >/dev/null 2>&1
  for _ in $(seq 1 10); do probe; sleep 0.3; done   # read while it's down
  echo "    restarting $VICTIM"
  $COMPOSE start "$VICTIM" >/dev/null 2>&1
  for _ in $(seq 1 10); do probe; sleep 0.3; done   # read while it recovers
done

echo "=== chaos complete: reads_ok=$reads_ok reads_bad=$reads_bad ==="
[ "$reads_bad" -eq 0 ] && echo "PASS: every read returned correct data through the chaos" \
                       || { echo "FAIL: $reads_bad bad reads"; exit 1; }
