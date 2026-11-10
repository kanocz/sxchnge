#!/bin/sh
# Encrypted interop check of the examples: C client -> Go server,
# Go client -> C server, C client -> C server. Needs port 3456 free.
# The libraries have their own tests: "go test ./go/" and "make -C c test".
set -e
cd "$(dirname "$0")"

tmp=$(mktemp -d)
pids=
trap 'kill $pids 2>/dev/null; rm -rf "$tmp"' EXIT

make -C c
make -B -C c/example/client
make -B -C c/example/server
go build -o "$tmp/go-server" ./go/example/server
go build -o "$tmp/go-client" ./go/example/client

# check NAME SERVER CLIENT PATTERN...: every pattern must be in the server log
check() {
  name=$1 server=$2 client=$3
  shift 3

  "$server" >"$tmp/log" 2>&1 &
  pids=$!
  sleep 1
  "$client" >"$tmp/client.log" 2>&1 &
  pids="$pids $!"
  # The C server buffers its output and writes it on exit, which comes with
  # the read timeout 3 s after the last message.
  sleep 5
  kill $pids 2>/dev/null || true
  wait $pids 2>/dev/null || true
  pids=

  for pattern; do
    if ! grep -q "$pattern" "$tmp/log"; then
      echo "--- server log"
      cat "$tmp/log"
      echo "--- client log"
      cat "$tmp/client.log"
      echo "interop FAILED: $name: no '$pattern' in the server log"
      exit 1
    fi
  done
  echo "interop OK: $name"
}

check "C client -> Go server" "$tmp/go-server" c/example/client/client-example \
  'String message received: Hello world!' \
  'Received somedata: &{ID:333 Value:12.345678}'

check "Go client -> C server" c/example/server/server-example "$tmp/go-client" \
  'String received: Hello, world!' \
  'SomeData received: ID: 123, Value: 3.141593' \
  'JSON received: {"ID":345,"Value":2.718281828459045}'

check "C client -> C server" c/example/server/server-example c/example/client/client-example \
  'String received: Hello world!' \
  'SomeData received: ID: 333, Value: 12.345678'
