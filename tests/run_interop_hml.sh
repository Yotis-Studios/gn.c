#!/bin/sh
# gn.hml client against a gn.c server (tests/server_peer.c).
# Usage: tests/run_interop_hml.sh <server_peer binary> [gn.hml path]
set -e
BIN="$1"
GNHML=$(cd "${2:-${GNHML:-../gn.hml}}" && pwd)
TMP=$(mktemp -d)
trap 'kill $PID 2>/dev/null; rm -rf "$TMP"' EXIT
"$BIN" 0 > "$TMP/server.log" &
PID=$!
i=0
until grep -q READY "$TMP/server.log"; do
    i=$((i + 1)); [ $i -gt 100 ] && { echo "server did not start"; exit 1; }
    sleep 0.1
done
PORT=$(sed -n 's/^READY //p' "$TMP/server.log")
sed "s|@GNHML@|$GNHML|" "$(dirname "$0")/interop_client.hml.in" > "$TMP/interop_client.hml"
timeout 60 hemlock "$TMP/interop_client.hml" "$PORT"
