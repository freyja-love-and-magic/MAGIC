#!/bin/sh
# End-to-end: real fount (run from a throwaway copy, so your fount data/ is untouched) behind a verifying
# proxy, driven by the C++ wand + gateway over libcurl. Needs node, a C++ compiler, libcurl.
#   FOUNT_DIR=/path/to/fount/src/server/node ./run.sh      (default: ../../../../../fount/src/server/node)
#   DIRECT=1 ./run.sh      talk to fount itself, with no verifying proxy: passes only if fount verifies signatures itself
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(cd "$HERE/../.." && pwd)
FOUNT_DIR=${FOUNT_DIR:-$KIT/../../../../fount/src/server/node}
[ -f "$FOUNT_DIR/fount.js" ] || { echo "fount not found at $FOUNT_DIR (set FOUNT_DIR)"; exit 2; }
DIRECT=${DIRECT:-0}
for p in 3006 3099; do
  if lsof -iTCP:$p -sTCP:LISTEN >/dev/null 2>&1; then echo "port $p is already in use"; exit 2; fi
done

TMP=$(mktemp -d)
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

mkdir -p "$TMP/fount"
rsync -a --exclude data --exclude node_modules "$FOUNT_DIR/" "$TMP/fount/"
ln -s "$FOUNT_DIR/node_modules" "$TMP/fount/node_modules"
# `exec` so $! is node itself, not a subshell (a subshell PID leaks the server when we clean up)
( cd "$TMP/fount" && exec env LOCALHOST=1 node fount.js >"$TMP/fount.log" 2>&1 ) &
PIDS="$PIDS $!"
if [ "$DIRECT" = "1" ]; then
  BASE=http://127.0.0.1:3006
  echo READY >"$TMP/proxy.log"   # no proxy in this mode
else
  BASE=http://127.0.0.1:3099
  node "$HERE/strict-proxy.mjs" >"$TMP/proxy.log" 2>&1 &
  PIDS="$PIDS $!"
fi

i=0
until curl -s -m 1 localhost:3006/api/ping >/dev/null 2>&1 && grep -q READY "$TMP/proxy.log" 2>/dev/null; do
  i=$((i+1)); [ $i -gt 40 ] && { echo "fount/proxy did not start"; tail -5 "$TMP/fount.log" "$TMP/proxy.log"; exit 2; }
  sleep 0.5
done

(cd "$KIT/test/host" && make build/e2e >/dev/null)
rc=0
"$KIT/test/host/build/e2e" "$BASE" || rc=$?

if [ "$DIRECT" = "1" ]; then
  [ $rc -eq 0 ] && echo "(direct mode: fount verified the signatures itself)"
  exit $rc
fi

echo "--- proxy decisions"
grep -E "^(ACCEPT|REJECT)" "$TMP/proxy.log" || true
acc=$(grep -c "^ACCEPT resolve" "$TMP/proxy.log" || true)
rej=$(grep -c "^REJECT resolve" "$TMP/proxy.log" || true)
echo "resolve accepted=$acc rejected=$rej (expected accepted=3 rejected=1; the oversized extra never reaches the proxy)"
[ "$acc" = "3" ] && [ "$rej" = "1" ] || rc=1
exit $rc
