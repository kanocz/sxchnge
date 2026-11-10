#define _DEFAULT_SOURCE // getaddrinfo and friends under -std=c11 on glibc

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <stdio.h>

#include "crc32.h"
#include "sxchnge.h"

// AES support is optional: without SX_ENCRYPTION the library does not need
// OpenSSL and refuses a connection that has AESKey set.
#ifdef SX_ENCRYPTION
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define LISTEN_BACKLOG 1024
#define headerHello                                                            \
  { 's', 'x', 'c', 'h', 'n', 'g', 1, 1 }

#define initialPacketSize 1292
struct initialPacket {
  char Header[8];
  uint32_t MaxSize;
  int8_t SizeBytes[256];
  uint32_t FixedSizes[256];
};
// the struct goes over the wire as it is, the Go version has the same layout
_Static_assert(sizeof(struct initialPacket) == initialPacketSize,
               "the compiler pads struct initialPacket");

// Writing to a socket the peer has closed must return an error, not kill the
// process with SIGPIPE: Linux takes a send() flag, macOS a socket option.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Idle time before the first keep-alive probe (TCP_KEEPALIVE is the macOS name)
#if defined(TCP_KEEPIDLE)
#define SX_KEEPIDLE TCP_KEEPIDLE
#elif defined(TCP_KEEPALIVE)
#define SX_KEEPIDLE TCP_KEEPALIVE
#endif

// The caller owns sock and closes it on failure.
static int setSockOptions(int sock, struct SXConnection *conn) {
  int yes = 1;
  // a child process started with exec does not inherit the connection
  if (fcntl(sock, F_SETFD, FD_CLOEXEC) < 0) {
    return -5;
  }
  // send and recv never block, the waiting is done in poll with a deadline.
  // MSG_DONTWAIT per call is not enough: on macOS a send that had to wait for
  // buffer space blocked with it (testWriteTimeout).
  int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -5;
  }
  // as in Go: a small frame must not wait for the ACK of the previous one
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(int)) < 0) {
    return -5;
  }
#ifdef SO_NOSIGPIPE
  if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(int)) < 0) {
    return -5;
  }
#endif

  if (conn->KeepAlive > 0) { // 0 = no keep-alive, as in the Go version
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(int)) < 0) {
      return -5;
    }
#ifdef SX_KEEPIDLE
    // without it the first probe waits for the system default (7200 s on
    // Linux)
    if (setsockopt(sock, IPPROTO_TCP, SX_KEEPIDLE, &conn->KeepAlive,
                   sizeof(int)) < 0) {
      return -5;
    }
#endif
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &conn->KeepAlive,
                   sizeof(int)) < 0) {
      return -5;
    }
  }

  return 0;
}

// The timeouts are deadlines on the monotonic clock, in milliseconds, and the
// sockets are non-blocking. SO_RCVTIMEO/SO_SNDTIMEO are not used: they restart
// with every signal, and whether they cover a whole call or each wait inside
// it differs between systems.
static int64_t nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The deadline tv from now; -1, no deadline, for a zero tv. A timeout below
// 1 ms counts as 1 ms.
static int64_t deadlineAfter(const struct timeval *tv) {
  int64_t ms = (int64_t)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
  return ms > 0 ? nowMs() + ms : -1;
}

// Waits until sock is readable (POLLIN) or writable (POLLOUT). Returns -1
// when the deadline has passed or poll failed; a signal is neither.
static int waitReady(int sock, short events, int64_t deadline) {
  struct pollfd pfd = {.fd = sock, .events = events};
  for (;;) {
    int ms = -1;
    if (deadline >= 0) {
      int64_t left = deadline - nowMs();
      if (left <= 0)
        return -1;
      ms = left > INT_MAX ? INT_MAX : (int)left;
    }
    int rc = poll(&pfd, 1, ms);
    if (rc > 0)
      return 0;
    if (rc < 0 && EINTR != errno)
      return -1;
  }
}

// Sends all of data before the deadline.
static int loopWrite(int sock, const uint8_t *data, size_t len,
                     int64_t deadline) {
  for (size_t i = 0; i < len;) {
    ssize_t x = send(sock, data + i, len - i, MSG_NOSIGNAL);
    if (x >= 0) {
      i += (size_t)x;
    } else if (EINTR == errno) {
      continue;
    } else if ((EAGAIN != errno && EWOULDBLOCK != errno) ||
               waitReady(sock, POLLOUT, deadline) < 0) {
      return -5;
    }
  }
  return 0;
}

// Reads exactly len bytes before the deadline. A closed connection and a
// timeout are errors, a signal is not.
static int recvAll(int sock, void *buf, size_t len, int64_t deadline) {
  for (size_t i = 0; i < len;) {
    ssize_t n = recv(sock, (uint8_t *)buf + i, len - i, 0);
    if (n > 0) {
      i += (size_t)n;
    } else if (0 == n) {
      return -1; // closed by the peer
    } else if (EINTR == errno) {
      continue;
    } else if ((EAGAIN != errno && EWOULDBLOCK != errno) ||
               waitReady(sock, POLLIN, deadline) < 0) {
      return -1;
    }
  }
  return 0;
}

static int encEnabled(struct SXConnection *conn) {
  return NULL != conn->AESKey && '\0' != conn->AESKey[0];
}

#ifdef SX_ENCRYPTION

// AES-CTR state of one socket, one context per direction. One SXConnection
// serves every socket returned by SXAccpet, so the state is keyed by fd.
// ponytail: fixed table, an encrypted socket needs fd < SX_MAX_ENC_FD; replace
// with a growable map if a process holds more descriptors than that.
#define SX_MAX_ENC_FD 65536
static struct {
  EVP_CIPHER_CTX *r, *w;
} ciphers[SX_MAX_ENC_FD];

static int encFd(int sock) { return sock >= 0 && sock < SX_MAX_ENC_FD; }

static void encReset(int sock) {
  if (!encFd(sock))
    return;
  EVP_CIPHER_CTX_free(ciphers[sock].r);
  EVP_CIPHER_CTX_free(ciphers[sock].w);
  ciphers[sock].r = ciphers[sock].w = NULL;
}

// Decodes a hex AES key, returns its length in bytes or -1.
static int parseKey(const char *hex, uint8_t key[32]) {
  size_t len = strlen(hex);
  if (len != 32 && len != 48 && len != 64)
    return -1;
  for (size_t i = 0; i < len; i += 2) {
    unsigned int byte;
    if (!isxdigit((unsigned char)hex[i]) ||
        !isxdigit((unsigned char)hex[i + 1]) ||
        sscanf(hex + i, "%2x", &byte) != 1)
      return -1;
    key[i / 2] = (uint8_t)byte;
  }
  return (int)(len / 2);
}

// 0, or the error code for an AESKey this build cannot use
static int encCheckKey(struct SXConnection *conn) {
  uint8_t key[32];
  int ok = !encEnabled(conn) || parseKey(conn->AESKey, key) > 0;
  OPENSSL_cleanse(key, sizeof(key));
  return ok ? 0 : -8;
}

static EVP_CIPHER_CTX *newCTR(const EVP_CIPHER *cipher, const uint8_t *key,
                              const uint8_t *iv) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (NULL != ctx && EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    ctx = NULL;
  }
  return ctx;
}

// Same exchange as encSetup in go/common.go: each side sends a random IV in
// the clear and encrypts its outgoing stream with it.
static int encSetup(int sock, struct SXConnection *conn) {
  encReset(sock); // the fd may have carried an encrypted connection before
  if (!encEnabled(conn))
    return 0;
  if (!encFd(sock))
    return -1;

  uint8_t key[32];
  const EVP_CIPHER *cipher;
  switch (parseKey(conn->AESKey, key)) {
  case 16:
    cipher = EVP_aes_128_ctr();
    break;
  case 24:
    cipher = EVP_aes_192_ctr();
    break;
  case 32:
    cipher = EVP_aes_256_ctr();
    break;
  default:
    return -1;
  }

  uint8_t iv[16], peerIV[16];
  int result = -1;
  if (RAND_bytes(iv, sizeof(iv)) == 1 &&
      loopWrite(sock, iv, sizeof(iv), deadlineAfter(&conn->WriteTimeout)) ==
          0 &&
      recvAll(sock, peerIV, sizeof(peerIV),
              deadlineAfter(&conn->ReadTimeout)) == 0) {
    ciphers[sock].w = newCTR(cipher, key, iv);
    ciphers[sock].r = newCTR(cipher, key, peerIV);
    if (NULL != ciphers[sock].w && NULL != ciphers[sock].r)
      result = 0;
    else
      encReset(sock);
  }
  OPENSSL_cleanse(key, sizeof(key));
  return result;
}

// XORs buf in place with the keystream of one direction of the socket; does
// nothing for a plain socket. CTR decryption is the same XOR as encryption.
// A key without cipher state (the socket was closed with SXClose, or did not
// come from SXConnect/SXAccpet) is an error: the data must not pass in the
// clear.
static int encCrypt(struct SXConnection *conn, int sock, int write,
                    uint8_t *buf, size_t len) {
  EVP_CIPHER_CTX *ctx = NULL;
  if (encFd(sock))
    ctx = write ? ciphers[sock].w : ciphers[sock].r;
  int out;
  if (NULL == ctx)
    return encEnabled(conn) ? -1 : 0;
  if (len > INT_MAX || EVP_EncryptUpdate(ctx, buf, &out, buf, (int)len) != 1)
    return -1;
  return 0;
}

#else // no SX_ENCRYPTION

static void encReset(int sock) { (void)sock; }

static int encCheckKey(struct SXConnection *conn) {
  return encEnabled(conn) ? -7 : 0;
}

static int encSetup(int sock, struct SXConnection *conn) {
  (void)sock;
  return encEnabled(conn) ? -1 : 0;
}

static int encCrypt(struct SXConnection *conn, int sock, int write,
                    uint8_t *buf, size_t len) {
  (void)conn, (void)sock, (void)write, (void)buf, (void)len;
  return 0;
}

#endif

// Encrypts buf in place when the socket has a cipher, then sends it.
static int sxWrite(struct SXConnection *conn, int sock, uint8_t *buf,
                   size_t len) {
  if (encCrypt(conn, sock, 1, buf, len) < 0)
    return -5;
  return loopWrite(sock, buf, len, deadlineAfter(&conn->WriteTimeout));
}

static int sxRead(struct SXConnection *conn, int sock, uint8_t *buf,
                  size_t len) {
  if (recvAll(sock, buf, len, deadlineAfter(&conn->ReadTimeout)) < 0)
    return -1;
  return encCrypt(conn, sock, 0, buf, len);
}

static void prepareInitialPacket(struct initialPacket *ip,
                                 struct SXConnection *conn) {
  static const char header[8] = headerHello;
  for (int i = 0; i < 8; i++)
    ip->Header[i] = header[i];
  if (encEnabled(conn))
    ip->Header[4] = 'E';
  ip->MaxSize = conn->MaxSize;
  for (int i = 0; i < 256; i++) {
    struct SXDataType *dt = conn->SXDataType[i];
    if (NULL != dt) {
      ip->FixedSizes[i] = dt->FixedSize;
      ip->SizeBytes[i] = dt->SizeBytes;
    } else {
      ip->FixedSizes[i] = 0;
      ip->SizeBytes[i] = 0;
    }
  }
}

static int connectionInit(int sock, struct SXConnection *conn) {
  // objects of the struct type, not char buffers cast to it: no alignment or
  // aliasing questions
  struct initialPacket hello, peer;
  prepareInitialPacket(&hello, conn);
  if (loopWrite(sock, (const uint8_t *)&hello, sizeof(hello),
                deadlineAfter(&conn->WriteTimeout)) < 0)
    return -1;
  if (recvAll(sock, &peer, sizeof(peer), deadlineAfter(&conn->ReadTimeout)) <
      0)
    return -2;
  if (0 != memcmp(&hello, &peer, sizeof(hello)))
    return -3;
  if (encSetup(sock, conn) < 0)
    return -4;
  return 0;
}

// The packet layout is checked by the _Static_assert above.
int SXInit(void) { return 0; }

// Resolves ip:port for TCP, NULL if it fails; free with freeaddrinfo.
// A NULL ip is the IPv4 wildcard address, for listening.
static struct addrinfo *resolve(const char *ip, uint16_t port, int flags) {
  char service[6];
  snprintf(service, sizeof(service), "%u", (unsigned)port);
  struct addrinfo hints = {.ai_family = NULL == ip ? AF_INET : AF_UNSPEC,
                           .ai_socktype = SOCK_STREAM,
                           .ai_flags = flags};
  struct addrinfo *res = NULL;
  return 0 == getaddrinfo(ip, service, &hints, &res) ? res : NULL;
}

// connect() that gives up at the deadline; without one (-1) the system
// default ends it. Never a blocking connect: a signal would fail it.
static int connectDeadline(int sock, const struct sockaddr *addr,
                           socklen_t addrlen, int64_t deadline) {
  int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    return -1;

  int rc = connect(sock, addr, addrlen);
  if (rc < 0 && EINPROGRESS == errno) {
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (0 == waitReady(sock, POLLOUT, deadline) &&
        0 == getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) && 0 == err)
      rc = 0;
    else
      rc = -1;
  }
  return rc; // the socket stays non-blocking, see setSockOptions
}

int SXConnect(const char *ip, uint16_t port, struct SXConnection *conn) {

  // after a failure the caller must not find the descriptor of an earlier,
  // closed connection here
  conn->socket = -1;

  int keyRc = encCheckKey(conn);
  if (keyRc < 0) {
    return keyRc;
  }

  struct addrinfo *res = resolve(ip, port, 0);
  if (NULL == res) {
    return -2;
  }

  // One deadline for all the addresses of the host. Each address gets an
  // equal share of the time that is left, as in Go: one that does not answer
  // must not use up the time of the next ones.
  int64_t deadline = deadlineAfter(&conn->ConnectTimeout);
  int left = 0;
  for (struct addrinfo *ai = res; NULL != ai; ai = ai->ai_next) {
    left++;
  }
  int sock = -1;
  for (struct addrinfo *ai = res; NULL != ai; ai = ai->ai_next, left--) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) {
      continue;
    }
    int64_t share = deadline;
    if (deadline >= 0) {
      int64_t now = nowMs();
      share = now + (deadline - now) / left;
    }
    if (0 == connectDeadline(sock, ai->ai_addr, ai->ai_addrlen, share)) {
      break;
    }
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);

  if (sock < 0) {
    return -4;
  }

  if (setSockOptions(sock, conn) < 0) {
    close(sock);
    return -5;
  }

  if (connectionInit(sock, conn) < 0) {
    close(sock);
    return -6;
  }

  conn->socket = sock;

  return 0;
}

int SXListen(const char *ip, uint16_t port, struct SXConnection *conn) {

  // as in SXConnect: after a failure SXClose(conn->socket) must not close a
  // descriptor that is not ours (0 in a zeroed struct)
  conn->socket = -1;

  // fail at startup, not on every accepted connection
  int keyRc = encCheckKey(conn);
  if (keyRc < 0) {
    return keyRc;
  }

  struct addrinfo *res = resolve(ip, port, AI_PASSIVE);
  if (NULL == res) {
    return -3;
  }

  int result = 0;
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  int reuseaddrVal = 1;
  if (-1 == sock) {
    result = -1;
  } else if (fcntl(sock, F_SETFD, FD_CLOEXEC) < 0 ||
             setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddrVal,
                        sizeof(reuseaddrVal)) == -1) {
    result = -2;
  } else if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
    result = -3;
  } else if (listen(sock, LISTEN_BACKLOG) < 0) {
    result = -4;
  }
  freeaddrinfo(res);

  if (result < 0) {
    if (-1 != sock) {
      close(sock);
    }
    return result;
  }

  conn->socket = sock;

  return 0;
}

int SXAccpet(struct SXConnection *conn) {
  int keyRc = encCheckKey(conn);
  if (keyRc < 0) {
    return keyRc;
  }

  int sock = accept(conn->socket, NULL, NULL);
  if (sock < 0) {
    return sock;
  }

  if (setSockOptions(sock, conn) < 0) {
    close(sock);
    return -5;
  }

  if (connectionInit(sock, conn) < 0) {
    close(sock);
    return -6;
  }

  return sock;
}

int SXClose(int sock) {
  encReset(sock);
  return close(sock);
}

int SXWriteMsg(struct SXConnection *conn, int sock, uint8_t msgType,
               const void *msg, uint32_t msgLen) {

  struct SXDataType *dt = conn->SXDataType[msgType];

  if (NULL == dt) {
    return -1;
  }

  uint8_t header[5];
  header[0] = msgType;
  uint8_t headerLen = 1;

  switch (dt->SizeBytes) {
  case -1:
    if (dt->FixedSize != msgLen) {
      return -2;
    }
    break;
  case 0:
    msgLen = 0;
    break;
  case 1:
    if (msgLen > 255) {
      return -3;
    }

    header[1] = (uint8_t)msgLen;
    headerLen = 2;
    break;
  case 2:
    if (msgLen > 0xffff) {
      return -3;
    }
    header[1] = (uint8_t)(msgLen & 0xff);
    header[2] = (uint8_t)((msgLen & 0xff00) >> 8);
    headerLen = 3;
    break;
  case 3:
    if (msgLen > 0xffffff) {
      return -3;
    }
    header[1] = (uint8_t)(msgLen & 0xff);
    header[2] = (uint8_t)((msgLen & 0xff00) >> 8);
    header[3] = (uint8_t)((msgLen & 0xff0000) >> 16);
    headerLen = 4;
    break;
  default:
    return -4;
  }

  // the peer has the same MaxSize and would drop the connection
  if ((conn->MaxSize > 0) && (msgLen > conn->MaxSize)) {
    return -3;
  }

  // One buffer and one send per frame: with TCP_NODELAY header, body and crc
  // sent separately are three TCP segments.
  size_t frameLen = headerLen + (size_t)msgLen + (msgLen > 0 ? 4 : 0);
  uint8_t *frame = malloc(frameLen);
  if (NULL == frame) {
    return -10;
  }
  memcpy(frame, header, headerLen);
  if (msgLen > 0) {
    uint32_t crc = sxCrc32(msg, msgLen);
    memcpy(frame + headerLen, msg, msgLen);
    memcpy(frame + headerLen + msgLen, &crc, sizeof(crc));
  }

  int result = sxWrite(conn, sock, frame, frameLen);
  free(frame);
  return result;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {

  uint8_t msgType = 0;

  if (sxRead(conn, sock, &msgType, 1) < 0) {
    return -2;
  }

  struct SXDataType *dt = conn->SXDataType[msgType];

  // SizeBytes is local config, but a value above 3 would let the peer write
  // past sizebuf; 4 is valid in the Go version and not implemented here.
  if (NULL == dt || dt->SizeBytes < -1 || dt->SizeBytes > 3) {
    return -1;
  }

  uint8_t sizebuf[4];
  if (dt->SizeBytes > 0) {
    if (sxRead(conn, sock, sizebuf, (size_t)dt->SizeBytes) < 0) {
      return -2;
    }
  }

  uint32_t size2read = 0;

  switch (dt->SizeBytes) {
  case -1:
    size2read = dt->FixedSize;
    break;
  //  case 0: // we just have 0 in size2read
  case 1:
    size2read = (uint32_t)sizebuf[0];
    break;
  case 2:
    size2read = (uint32_t)sizebuf[0] | (((uint32_t)sizebuf[1]) << 8);
    break;
  case 3:
    size2read = (uint32_t)sizebuf[0] | (((uint32_t)sizebuf[1]) << 8) |
                (((uint32_t)sizebuf[2]) << 16);
    break;
  }

  // MaxSize bounds what an unauthenticated peer can make us allocate per message.
  if ((conn->MaxSize > 0) && (size2read > conn->MaxSize)) {
    return -4;
  }

  uint8_t *buf = NULL;
  if (size2read > 0) {
    size_t total = (size_t)size2read + 4; // payload + crc, no uint32 wrap
    buf = malloc(total);
    if (NULL == buf) {
      return -10;
    }
    if (sxRead(conn, sock, buf, total) < 0) {
      free(buf);
      return -2;
    }
    uint32_t crc; // the crc is not aligned inside buf
    memcpy(&crc, buf + size2read, sizeof(crc));
    if (crc != sxCrc32(buf, size2read)) {
      free(buf);
      return -3;
    }
  }

  if (dt->Callback) {
    dt->Callback(buf, size2read, sock, conn);
  }

  if (buf) {
    free(buf);
  }

  return 0;
}
