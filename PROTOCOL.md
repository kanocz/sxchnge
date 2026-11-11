# sxchnge protocol 2.0

One TCP connection, a client (`Connect`, `SXConnect`) and a server (`ListenOne`, `SXAccpet`). Every integer is little-endian. A peer that speaks protocol 1.1 does not get through the handshake: it sees another version in the hello and closes the connection.

## Hello

Each side sends its hello as soon as the connection is open and then reads the hello of the peer. 1324 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | `s x c h n g 2 0`; byte 4 is `E` instead of `n` when a key is set |
| 8 | 32 | nonce: random bytes with a key, zero without |
| 40 | 4 | `MaxSize` |
| 44 | 256 | `SizeBytes` of every type as a signed byte; -128 for a type that is not defined |
| 300 | 1024 | `FixedSize` of every type, 4 bytes each; 0 for a type that is not defined |

Everything except the nonce has to be equal on both sides, or the connection is closed.

## With a key

The shared key is 16, 24 or 32 bytes. It has to be random (`openssl rand -hex 32`): somebody who records a handshake can test guesses against it offline, which is hopeless for 128 random bits and easy for a password.

### Keys

```
key client→server = HMAC-SHA256(shared key, "sxchnge2 client" || client hello || server hello)
key server→client = HMAC-SHA256(shared key, "sxchnge2 server" || client hello || server hello)
```

Both are AES-256-GCM keys. Each direction counts its records from 0. The 12-byte GCM nonce of a record is its number in 8 bytes followed by 4 zero bytes.

### Key confirmation

After the hello each side sends record 0 of its direction, with no data and no additional data: the 16-byte tag alone. Then it reads the tag of the peer and verifies it. A failure means that the peer has another key, or that a hello was changed on the way: the keys depend on both hello packets.

### Messages

Records 1, 2, ... of a direction, one per message:

| Size | Field |
| --- | --- |
| 4 | n = length of the data + 1, in the clear |
| n | the data followed by the type byte, encrypted |
| 16 | tag |

The 4 length bytes are the additional data of the record. The receiver refuses n = 0 and n - 1 > `MaxSize` before it reads the rest. After the tag is verified it refuses a type that is not defined and a length the type does not allow: not `FixedSize` for `SizeBytes` -1, not 0 for `SizeBytes` 0, 2^(8·`SizeBytes`) or more for `SizeBytes` 1 to 3. The data of one message is at most 2^32 - 2 bytes.

The type byte comes last so that the data starts at the beginning of the buffer and keeps its alignment.

### What this gives

Somebody in the path without the key cannot pass the handshake, read a message, change one, add one, repeat one, reorder or drop one without the receiver closing the connection, and cannot replay a recorded session: the nonce of the other side gives new keys. A record is only good for its direction.

Not covered:

- The length of every message and the hello with the type table are visible.
- Sessions recorded today can be read by somebody who gets the key later (no forward secrecy).
- A peer that has the key is trusted completely.
- The two sides have no names. The handshake proves that the peer has the key, not which holder of the key it is: somebody in the path can connect a client to another server with the same key and the same hello fields, also to a server in the process of the client itself. Use one key for one link.
- Somebody in the path can still close the connection or hold data back until `ReadTimeout`. Closing it early cuts off the last messages, and the receiver sees the same as when the sender closes.

## Without a key

No confirmation. One message:

| Size | Field |
| --- | --- |
| 1 | type |
| `SizeBytes` (0 to 4) | length of the data; absent for `SizeBytes` -1 and 0 |
| length | data |
| 4 | CRC-32 of the data, Koopman polynomial (`crc32.Koopman` in Go); absent when there is no data |

This protects against line noise only. Anybody who can reach the port can pass the handshake by sending the hello back.
