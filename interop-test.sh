#!/bin/sh
# Encrypted interop check: the C example client talks to the Go example server.
# Needs port 3456 free.
set -e
cd "$(dirname "$0")"

tmp=$(mktemp -d)
trap 'kill $pid 2>/dev/null; rm -rf "$tmp"' EXIT

make -C c
make -B -C c/example/client
go build -o "$tmp/server" ./go/example/server

"$tmp/server" >"$tmp/log" 2>&1 &
pid=$!
sleep 1
c/example/client/client-example
sleep 1

if grep -q 'String message received: Hello world!' "$tmp/log" &&
   grep -q 'Received somedata: &{ID:333 Value:12.345678}' "$tmp/log"; then
  echo "interop OK"
else
  cat "$tmp/log"
  echo "interop FAILED"
  exit 1
fi
