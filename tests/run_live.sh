#!/bin/sh
# Starts tests/server.js (gn.js) and runs a test_client binary against it.
# Usage: tests/run_live.sh <test_client binary> [gn.js path]
set -e
BIN="$1"
GNJS="${2:-${GNJS:-../gn.js}}"
WS_PORT=${WS_PORT:-18431}
HTTP_PORT=$((WS_PORT + 1))
CLOSED_PORT=$((WS_PORT + 2))
LOG=$(mktemp)
node "$(dirname "$0")/server.js" "$GNJS" "$WS_PORT" "$HTTP_PORT" > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; rm -f "$LOG"' EXIT
i=0
until grep -q READY "$LOG"; do
    i=$((i + 1))
    if [ $i -gt 100 ] || ! kill -0 $PID 2>/dev/null; then
        echo "test server did not start:"; cat "$LOG"; exit 1
    fi
    sleep 0.1
done
$RUNNER "$BIN" "$WS_PORT" "$HTTP_PORT" "$CLOSED_PORT"
