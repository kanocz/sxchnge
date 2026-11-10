#!/bin/sh
# Interop check of the examples: C client -> Go server, Go client -> C server,
# C client -> C server. Runs twice: the C side built with ENCRYPTION=1 and an
# AES key, then the default C build without a key. Needs port 3456 free.
# The libraries have their own tests: "go test ./go/" and "make -C c test".
set -e
cd "$(dirname "$0")"

tmp=$(mktemp -d)
pids=
trap 'kill $pids 2>/dev/null; rm -rf "$tmp"' EXIT

go build -o "$tmp/go-server" ./go/example/server
go build -o "$tmp/go-client" ./go/example/client

# check NAME SERVER CLIENT PATTERN...: every pattern must be in the server log.
# $key, when set, is passed to both sides.
check() {
  name=$1 server=$2 client=$3
  shift 3

  "$server" $key >"$tmp/log" 2>&1 &
  pids=$!
  sleep 1
  "$client" $key >"$tmp/client.log" 2>&1 &
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
      echo "interop FAILED: $name ($mode): no '$pattern' in the server log"
      exit 1
    fi
  done
  echo "interop OK: $name ($mode)"
}

# round MODE MAKE_ARGUMENT KEY: rebuilds the C side and checks the three pairs
round() {
  mode=$1 key=$3
  make -B -C c $2
  make -B -C c/example/client $2
  make -B -C c/example/server $2

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
}

round encrypted ENCRYPTION=1 00112233445566778899AABBCCDDEEFF
round plain "" "" # last, so the tree is left with the default build
