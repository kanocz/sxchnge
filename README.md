# sxchnge
Easy exchange struct and plain data via TCP between golang and C

Protocol providing handshake on connect, AES-CTR encryption, comparing version and list of datatypes, sizes etc

Encryption is enabled by setting `AESKey` (AES-128/192/256 key in hex) to the same value on both sides.

The C version is built without encryption by default and then has no dependencies. Build it with `make ENCRYPTION=1` (the compiler flag `-DSX_ENCRYPTION`) to get AES support; that build uses OpenSSL libcrypto, link your program with `-lcrypto`.

## Rules for both versions

- The handshake fails unless both sides have the same `MaxSize`, the same types (`SizeBytes` and `FixedSize` of all 256 slots) and the same encryption setting. One difference passes: a type with `SizeBytes` 0 looks the same as an undefined slot, so a no-data type defined on one side only is found when its first message arrives.
- `SizeBytes` of a type: `-1` fixed size of `FixedSize` bytes, `0` no data, `1` to `3` a size header of that many bytes. The Go version also accepts `4`, the C version does not.
- `MaxSize` limits one message in both directions. Zero differs: Go rejects every non-empty message, C applies no limit.
- A connection that receives nothing for `ReadTimeout` is closed. Send a ping type more often than that to keep it open.
- `KeepAlive` is the idle time before the first TCP keep-alive probe and the interval between probes. Zero turns keep-alive off.
- Zero timeouts differ: in Go a zero `ReadTimeout` or `WriteTimeout` fails every read or write, in C zero means no timeout.
- After a failed read or write the stream is in the middle of a message. Close the connection and open a new one.
- The handshake and the CRC use host byte order, so both hosts have to be little-endian.

## Go

```go
c := sxchange.Connection{MaxSize: 1024, ReadTimeout: 3 * time.Second, WriteTimeout: 3 * time.Second, Types: types}

err := c.Connect("host:3456")                      // client
err := c.ListenOne(":3456", onConnect, onError)    // server, one client at a time
err := c.WriteMsg(msgType, data)
c.Disconnect()
err := c.Close()
```

- The module needs Go 1.23 or newer.
- `WriteMsg` can be called from several goroutines. A failed write closes the connection: part of the frame may be on the wire, so every later `WriteMsg` returns an error until the next connection.
- Callbacks run one at a time in the read loop. After `Connect` on a connected client the first callback of the new connection waits until a callback of the old one has returned. The slice they get is reused for the next message: copy it to keep it.
- `Disconnect()` closes the current connection. A blocked read returns at once and no callback runs after it. A `ListenOne` server goes on to accept the next client.
- `Close()` does the same and also stops a `ListenOne` server. The `Connection` can then be used for `Connect` or `ListenOne` again.
- `Connect` on a `Connection` that is already connected closes the previous connection first.
- `ListenOne` calls `onConnect` in a new goroutine for every client; the channel it gets is closed when that connection ends. Errors of the connection, a panic in a callback included (with its stack trace), go to `onError`. Both callbacks are required.

Full programs: [go/example](go/example).

## C

The functions and their return codes are described in [c/sxchnge.h](c/sxchnge.h); full programs are in [c/example](c/example).

```c
SXInit();
SXListen("127.0.0.1", 3456, &conn);   // NULL instead of the address: every IPv4 interface
int sock = SXAccpet(&conn);
SXProcessMsg(&conn, sock);            // reads one message, calls its callback
SXWriteMsg(&conn, sock, type, data, len);
SXClose(sock);
```

- One `struct SXConnection` serves every socket accepted from its listener.
- Per socket one thread may read while another one writes. Two writers on the same socket need a lock in the calling code.
- Close the sockets with `SXClose`, not `close`: it also frees the AES state of the socket.
- An encrypted socket needs a descriptor below 65536.
- `ReadTimeout` applies to each read of `SXProcessMsg` (the type byte, the size, the data with its CRC), so the first one is the wait for a new message. `WriteTimeout` covers one whole frame.
- `ConnectTimeout` covers the TCP connects to all the addresses of the host together; each address gets an equal share of the time that is left, so one that does not answer leaves time for the next. Resolving a host name is not included.
- A signal neither fails a call nor restarts its timeout, with one exception: `SXAccpet` waiting for a client returns `-1` with `errno` `EINTR`. A handler that sets a flag, installed without `SA_RESTART`, is the way to stop an accept loop. `ECONNABORTED` from `SXAccpet` is no reason to stop a server either.
- The sockets are non-blocking and have close-on-exec set. The library waits in `poll`; a program that calls `send` or `recv` on such a socket itself gets `EAGAIN` instead of waiting.

Build options:

| Command | Result |
| --- | --- |
| `make -C c` | no encryption, no OpenSSL; `SXConnect`, `SXListen` and `SXAccpet` return `-7` when `AESKey` is set |
| `make -C c ENCRYPTION=1` | AES support, needs the OpenSSL headers and `-lcrypto` at link time |

`make` does not notice a changed `ENCRYPTION`: add `-B` when switching. `struct SXConnection` is the same in both builds, so the define is needed only for `sxchnge.c`, not for the code that includes `sxchnge.h`.

All four example programs take the AES key as an optional argument, for example `server-example 00112233445566778899AABBCCDDEEFF`.

## Changes after v1.1.3 that need attention in the calling code

The wire format is the same: old and new versions talk to each other.

Go:

- `Connection.CloseChan` is gone. Replace `close(c.CloseChan)` with `c.Disconnect()`, or with `c.Close()` to stop a `ListenOne` server too.
- `WriteMsg` returns an error for a message longer than `MaxSize`. Before, the message was sent and the peer dropped the connection.
- `WriteMsg` before `Connect` returns an error instead of a nil pointer panic.
- The handshake now compares `SizeBytes`. Two Go programs that define the same type with different `SizeBytes` no longer connect.
- A panic in a callback reaches `onError` together with its stack trace; before, `ListenOne` reported nothing. The library no longer prints the panic to stdout.
- `ListenOne` returns an error when `onConnect` or `onError` is nil. Before, the first failed handshake crashed the program.
- A failed `WriteMsg` closes the connection. Before, the next `WriteMsg` returned nil although its frame followed half of the previous one and was lost.
- `WriteMsg` during a handshake returns an error at once. Before, it waited for the handshake, up to `ReadTimeout` with a silent peer.
- The size limits are 65535 bytes for `SizeBytes` 2 and 16777215 for 3 (they were 65025 and 16581375).
- `KeepAlive` 0 turns keep-alive off. Before, the default of the Go runtime stayed on: probes after 15 s of silence.
- `Connect` and `ListenOne` return an error for an invalid `AESKey` before any network activity. Before, `ListenOne` started and reported the key through `onError` on every connection.
- The module needs Go 1.23 (it was 1.16), for `net.KeepAliveConfig`. Built with Go 1.23 or newer, the old code set only the idle time from `KeepAlive`, and the probe interval stayed at the 15 s of the Go runtime.
- Callbacks of a client never overlap. Before, `Connect` on a connected client could start a callback of the new connection while one of the old connection was still running. A callback that never returns now blocks the callbacks of the next connection.
- The package compiles for 32-bit platforms; `WriteMsg` did not.

C:

- Encryption is compiled in only with `-DSX_ENCRYPTION` (`make ENCRYPTION=1`). A program that sets `AESKey` and links a library built without it gets `-7` from `SXConnect`, `SXListen` and `SXAccpet`.
- `SXListen(port, conn)` became `SXListen(ip, port, conn)`. Pass `NULL` as `ip` for the old behaviour.
- `SXClose(sock)` is new. Replace `close()` on sockets from `SXConnect`, `SXAccpet` and `SXListen` with it.
- `struct SXConnection` has a new last field `ConnectTimeout`; zero keeps the system default. The struct grew, so recompile everything that includes the header.
- `SXConnect` resolves the host with `getaddrinfo` and accepts IPv6 addresses. It no longer returns `-1` and `-3`: a failed connection to every resolved address is `-4`.
- `SXListen` returns `-3` also for an address that does not resolve.
- `SXWriteMsg` returns `-3` for a message longer than `MaxSize` and `-10` when out of memory. It sends a frame with one `send` instead of three.
- `SXProcessMsg` returns `-1` for a type with `SizeBytes` outside `-1..3`.
- `KeepAlive` is now also the idle time before the first probe (it was the system default, 7200 s on Linux). Zero turns keep-alive off.
- Writing to a connection closed by the peer returns `-5`; before, the process got SIGPIPE.
- The size limits are 65535 bytes for `SizeBytes` 2 and 16777215 for 3 (both were 65025).
- `TCP_NODELAY` is set on every connection, as in the Go version.
- `SXListen`, `SXConnect` and `SXAccpet` return `-8` for an invalid `AESKey` before any I/O. Before, `SXListen` succeeded and every `SXAccpet` returned `-6`, the code of a failed handshake.
- The timeouts are deadlines kept by the library; `SO_RCVTIMEO` and `SO_SNDTIMEO` are no longer set. `ReadTimeout` covers a whole read and `WriteTimeout` a whole frame on every system: before, a peer that sent or took a few bytes at a time could stretch them, and so could signals. A signal in the middle of a message no longer fails `SXProcessMsg`.
- The sockets returned by `SXConnect` and `SXAccpet` are non-blocking, and all sockets of the library have close-on-exec set.
- `ConnectTimeout` covers all the addresses of a host together, each address with an equal share of the time that is left. It does not cover the name lookup. A value below 1 ms counts as 1 ms.
- A signal during the connect no longer fails `SXConnect`. `SXAccpet` still returns `-1` with `errno` `EINTR` when a signal arrives while it waits for a client.
- A failed `SXConnect` or `SXListen` sets `conn->socket` to `-1`. Before, the old value stayed there: the descriptor of an earlier connection, or 0 in a zeroed struct.
- `SXWriteMsg` takes `const void *msg`; the `(char *)` casts in the calling code are no longer needed.
- `SXInit` always returns 0: the packet layout is checked when the library is compiled.
- With `AESKey` set, a socket without cipher state (closed with `SXClose`, or not returned by `SXConnect` or `SXAccpet`) gets `-5` from `SXWriteMsg` and `-2` from `SXProcessMsg`. Before, the data passed unencrypted.

## Tests

```sh
go test -race ./go/              # Go library
GOOS=linux GOARCH=386 go vet ./go/   # the Go library compiles for a 32-bit platform
make -B -C c test                # C library without encryption
make -B -C c ENCRYPTION=1 test   # C library with encryption
make -B -C c SAN=address,undefined test   # the same tests under sanitizers; SAN=thread for TSan
./interop-test.sh                # C and Go examples against each other, plain and encrypted; needs port 3456
```
