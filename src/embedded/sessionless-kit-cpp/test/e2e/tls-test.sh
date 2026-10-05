#!/bin/sh
# Proves the resolver verifies TLS certificates by default. Needs node, openssl, libcurl.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(cd "$HERE/../.." && pwd)
PORT=${TLS_TEST_PORT:-3443}
TMP=$(mktemp -d)
PID=""
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; rm -rf "$TMP"' EXIT INT TERM

cat >"$TMP/openssl.cnf" <<CNF
[req]
distinguished_name=dn
x509_extensions=v3
prompt=no
[dn]
CN=localhost
[v3]
subjectAltName=DNS:localhost
basicConstraints=CA:TRUE
CNF
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 1 -config "$TMP/openssl.cnf" >/dev/null 2>&1
node "$HERE/tls-server.mjs" "$TMP/cert.pem" "$TMP/key.pem" "$PORT" >"$TMP/server.log" 2>&1 &
PID=$!
i=0; until grep -q READY "$TMP/server.log" 2>/dev/null; do i=$((i+1)); [ $i -gt 20 ] && { echo "tls server did not start"; cat "$TMP/server.log"; exit 2; }; sleep 0.25; done

(cd "$KIT/test/host" && make build/tls_test >/dev/null)
rc=0
"$KIT/test/host/build/tls_test" "$PORT" "$TMP/cert.pem" || rc=$?
n=$(grep -c '^REQUEST' "$TMP/server.log" || true)
echo "requests that actually reached the server: $n (expected 2: trusted-CA and explicit-insecure; default and wrong-host never connected)"
[ "$n" = "2" ] || rc=1
exit $rc
