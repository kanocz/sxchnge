#ifndef SXCHNGE_H
#define SXCHNGE_H

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

struct SXConnection;

// data points into a buffer that is freed when the callback returns
typedef void (*SXCB)(void *data, size_t size, int sock,
                     struct SXConnection *conn);

struct SXDataType {
  int8_t SizeBytes; // -1 fixed size, 0 no data, 1..4 bytes of size header
  uint32_t FixedSize;
  SXCB Callback;
};

// Threads: one SXConnection can serve many sockets, and it is only read after
// SXListen/SXConnect. Per socket one thread may read (SXProcessMsg) while
// another one writes (SXWriteMsg); two writers on one socket need a lock in
// the caller, and so does SXClose against a thread that still uses the socket.
//
// Timeouts: a zero timeout means no limit, except in the handshake, where a
// zero ReadTimeout or WriteTimeout counts as 10 s. A signal neither fails a
// call nor restarts its timeout, with one exception: SXAccpet waiting for a
// client.
struct SXConnection {
  // set by SXConnect (the connection) and SXListen (the listener), -1 if the
  // call failed
  int socket;
  struct SXDataType *SXDataType[256];
  int KeepAlive; // seconds before the first probe and between probes; 0 = off
  // for each read of SXProcessMsg (the type, the size, the data with its crc;
  // with a key the length, then the record) and of the handshake; the first
  // one is the wait for a new message
  struct timeval ReadTimeout;
  struct timeval WriteTimeout; // for one whole frame
  uint32_t MaxSize; // of the data of one message; 0 = only messages without data
  // 16, 24 or 32 random bytes in hex (openssl rand -hex 32), not a password;
  // NULL or "" = no encryption. With a key every message is encrypted and
  // authenticated (AES-256-GCM). The handshake decides it for the socket:
  // clearing AESKey afterwards does not turn the encryption off. A key needs
  // the library compiled with -DSX_ENCRYPTION (make ENCRYPTION=1), which links
  // OpenSSL libcrypto. The struct is the same in both builds.
  const char *AESKey;
  // for the TCP connects of SXConnect, shared between the addresses of the
  // host; 0 = system default. Resolving a host name is not included: it takes
  // as long as the resolver of the system does.
  struct timeval ConnectTimeout;
};

// Returns 0; stays for the programs that call it.
int SXInit(void);

// SXConnect, SXListen and SXAccpet return -7 when AESKey is set and the
// library was compiled without SX_ENCRYPTION, and -8 before any I/O when
// AESKey is not 32, 48 or 64 hex digits. SXConnect and SXAccpet return -9
// when the key confirmation of the handshake fails: the peer has another
// AESKey, or somebody changed the handshake on the way.

// Connects and does the handshake, the socket is in conn->socket.
// -2 host not resolved, -4 no address connected, -5 socket options,
// -6 handshake (the peer has another protocol version, MaxSize, set of types
// or encryption setting).
int SXConnect(const char *ip, uint16_t port, struct SXConnection *conn);

// Listens on ip, NULL for every IPv4 interface; the socket is in conn->socket.
// -1 socket, -2 socket options, -3 address not resolved or busy, -4 listen.
int SXListen(const char *ip, uint16_t port, struct SXConnection *conn);

// Accepts one connection and does the handshake. Returns the socket,
// or a negative value: -5 socket options, -6 handshake, -1: accept failed,
// errno is the one of accept(). Two of them are no reason to stop a server:
// ECONNABORTED (the client left before it was accepted) and EINTR. A signal
// ends the wait for a client with EINTR, so a handler that sets a flag,
// installed without SA_RESTART, is the way to stop an accept loop.
int SXAccpet(struct SXConnection *conn);

// Closes a socket returned by SXConnect, SXAccpet or SXListen and frees its
// encryption state. Use it instead of close().
int SXClose(int sock);

// A non-zero result of SXWriteMsg or SXProcessMsg (a read timeout included)
// leaves the stream in the middle of a message: SXClose the socket, don't
// retry.

// -1 type not defined, -2 msgLen differs from FixedSize or is not 0 for a
// type without data, -3 message longer than the size header or MaxSize
// allows, -4 invalid SizeBytes, -5 write failed or timed out (also: AESKey is
// set and sock has no cipher, see SXClose), -10 out of memory.
int SXWriteMsg(struct SXConnection *conn, int sock, uint8_t msgType,
               const void *msg, uint32_t msgLen);

// Reads one message and calls its callback.
// -1 type not defined or invalid SizeBytes, -2 read failed (timeout, closed
// connection), -3 crc mismatch; with a key: the message is not authentic
// (changed, repeated, dropped or reordered on the way) or sock has no cipher,
// -4 message longer than MaxSize or of a size its type does not allow (with a
// key an invalid SizeBytes is reported here, not as -1), -10 out of memory.
int SXProcessMsg(struct SXConnection *conn, int sock);

#endif
