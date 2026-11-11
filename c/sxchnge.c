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
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#define LISTEN_BACKLOG 1024

// The wire format is described in PROTOCOL.md. Offsets in the hello packet:
#define HELLO_SIZE 1324
#define HELLO_NONCE 8 // 32 random bytes with a key, zero without
#define HELLO_MAXSIZE 40
#define HELLO_SIZEBYTES 44
#define HELLO_FIXEDSIZE 300

#define NONCE_SIZE 32
#define UNDEFINED_TYPE 0x80 // SizeBytes of a slot without a type: -128
#define TAG_SIZE 16         // AES-GCM tag

// Limit for the handshake when ReadTimeout or WriteTimeout is zero: a silent
// peer must not hold SXAccpet forever.
#define HANDSHAKE_TIMEOUT_MS 10000

// Integers go over the wire in little-endian order.
static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

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

// The same for the handshake, where a zero tv is HANDSHAKE_TIMEOUT_MS.
static int64_t handshakeDeadline(const struct timeval *tv) {
  int64_t deadline = deadlineAfter(tv);
  return deadline >= 0 ? deadline : nowMs() + HANDSHAKE_TIMEOUT_MS;
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

// AES-GCM state of one socket: a context with the key of each direction and
// the number of its next record, which is the nonce. One SXConnection serves
// every socket returned by SXAccpet, so the state is keyed by fd.
// ponytail: fixed table, an encrypted socket needs fd < SX_MAX_ENC_FD; replace
// with a growable map if a process holds more descriptors than that.
#define SX_MAX_ENC_FD 65536
static struct {
  EVP_CIPHER_CTX *r, *w;
  uint64_t rseq, wseq;
} ciphers[SX_MAX_ENC_FD];

static int encFd(int sock) { return sock >= 0 && sock < SX_MAX_ENC_FD; }

// Whether sock carries records. The handshake decides, not AESKey at the time
// of the call: a connection made with a key never falls back to frames in the
// clear. A key without cipher state counts too, and fails in encRecord.
static int encActive(struct SXConnection *conn, int sock) {
  return encEnabled(conn) || (encFd(sock) && NULL != ciphers[sock].w);
}

static void encReset(int sock) {
  if (!encFd(sock))
    return;
  EVP_CIPHER_CTX_free(ciphers[sock].r);
  EVP_CIPHER_CTX_free(ciphers[sock].w);
  memset(&ciphers[sock], 0, sizeof(ciphers[sock]));
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

static int encRandom(uint8_t *buf, int len) {
  return RAND_bytes(buf, len) == 1 ? 0 : -1;
}

// The cipher of one direction. Its AES-256-GCM key is the HMAC-SHA256 of the
// label and the hello packets of both sides under the shared key.
#define LABEL_SIZE 15
static EVP_CIPHER_CTX *newDirection(const uint8_t *psk, int pskLen,
                                    const char *label,
                                    const uint8_t *helloClient,
                                    const uint8_t *helloServer, int encrypt) {
  uint8_t msg[LABEL_SIZE + 2 * HELLO_SIZE], key[EVP_MAX_MD_SIZE];
  memcpy(msg, label, LABEL_SIZE);
  memcpy(msg + LABEL_SIZE, helloClient, HELLO_SIZE);
  memcpy(msg + LABEL_SIZE + HELLO_SIZE, helloServer, HELLO_SIZE);

  EVP_CIPHER_CTX *ctx = NULL;
  if (NULL != HMAC(EVP_sha256(), psk, pskLen, msg, sizeof(msg), key, NULL)) {
    ctx = EVP_CIPHER_CTX_new();
    if (NULL != ctx && EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, key,
                                         NULL, encrypt) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      ctx = NULL;
    }
  }
  OPENSSL_cleanse(key, sizeof(key));
  return ctx;
}

// One AES-GCM record, in place, with the next nonce of its direction. With
// write it encrypts buf and stores the tag; without, it decrypts buf and
// returns -1 unless the tag fits. aad is authenticated only.
static int encRecord(int sock, int write, const uint8_t *aad, int aadLen,
                     uint8_t *buf, size_t len, uint8_t *tag) {
  if (!encFd(sock))
    return -1;
  EVP_CIPHER_CTX *ctx = write ? ciphers[sock].w : ciphers[sock].r;
  uint64_t *seq = write ? &ciphers[sock].wseq : &ciphers[sock].rseq;
  if (NULL == ctx)
    return -1; // closed with SXClose, or not from SXConnect/SXAccpet

  uint8_t nonce[12] = {0}, none[16];
  for (int i = 0; i < 8; i++)
    nonce[i] = (uint8_t)(*seq >> (8 * i));
  (*seq)++;

  int out;
  if (EVP_CipherInit_ex(ctx, NULL, NULL, NULL, nonce, -1) != 1)
    return -1;
  if (!write &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag) != 1)
    return -1;
  if (aadLen > 0 && EVP_CipherUpdate(ctx, NULL, &out, aad, aadLen) != 1)
    return -1;
  for (size_t done = 0; done < len;) { // the length argument is an int
    int chunk = len - done > (1 << 30) ? (1 << 30) : (int)(len - done);
    if (EVP_CipherUpdate(ctx, buf + done, &out, buf + done, chunk) != 1)
      return -1;
    done += (size_t)chunk;
  }
  if (EVP_CipherFinal_ex(ctx, none, &out) != 1)
    return -1; // on read: the tag does not fit
  if (write &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) != 1)
    return -1;
  return 0;
}

// Derives the keys of both directions and exchanges the key confirmation:
// record 0 of each direction, without data. The keys depend on both hello
// packets, so it also proves that nobody changed them on the way.
// -2: the confirmation of the peer does not verify.
static int encSetup(int sock, struct SXConnection *conn,
                    const uint8_t *helloClient, const uint8_t *helloServer,
                    int server) {
  encReset(sock); // the fd may have carried an encrypted connection before
  if (!encEnabled(conn))
    return 0;
  if (!encFd(sock))
    return -1;

  uint8_t psk[32];
  int pskLen = parseKey(conn->AESKey, psk);
  if (pskLen < 0)
    return -1;
  EVP_CIPHER_CTX *toServer = newDirection(
      psk, pskLen, "sxchnge2 client", helloClient, helloServer, !server);
  EVP_CIPHER_CTX *toClient = newDirection(
      psk, pskLen, "sxchnge2 server", helloClient, helloServer, server);
  OPENSSL_cleanse(psk, sizeof(psk));
  ciphers[sock].w = server ? toClient : toServer;
  ciphers[sock].r = server ? toServer : toClient;

  uint8_t tag[TAG_SIZE], peerTag[TAG_SIZE];
  int result = -1;
  if (NULL != toServer && NULL != toClient &&
      encRecord(sock, 1, NULL, 0, NULL, 0, tag) == 0 &&
      loopWrite(sock, tag, sizeof(tag),
                handshakeDeadline(&conn->WriteTimeout)) == 0 &&
      recvAll(sock, peerTag, sizeof(peerTag),
              handshakeDeadline(&conn->ReadTimeout)) == 0)
    result = encRecord(sock, 0, NULL, 0, NULL, 0, peerTag) == 0 ? 0 : -2;
  if (result < 0)
    encReset(sock);
  return result;
}

#else // no SX_ENCRYPTION

static void encReset(int sock) { (void)sock; }

static int encActive(struct SXConnection *conn, int sock) {
  (void)sock;
  return encEnabled(conn); // fails in encRecord
}

static int encCheckKey(struct SXConnection *conn) {
  return encEnabled(conn) ? -7 : 0;
}

static int encRandom(uint8_t *buf, int len) {
  (void)buf, (void)len;
  return -1;
}

static int encRecord(int sock, int write, const uint8_t *aad, int aadLen,
                     uint8_t *buf, size_t len, uint8_t *tag) {
  (void)sock, (void)write, (void)aad, (void)aadLen, (void)buf, (void)len;
  (void)tag;
  return -1;
}

static int encSetup(int sock, struct SXConnection *conn,
                    const uint8_t *helloClient, const uint8_t *helloServer,
                    int server) {
  (void)sock, (void)helloClient, (void)helloServer, (void)server;
  return encEnabled(conn) ? -1 : 0;
}

#endif

static int prepareHello(uint8_t *hello, struct SXConnection *conn) {
  static const uint8_t header[8] = {'s', 'x', 'c', 'h', 'n', 'g', 2, 0};
  memset(hello, 0, HELLO_SIZE);
  memcpy(hello, header, sizeof(header));
  if (encEnabled(conn)) {
    hello[4] = 'E';
    if (encRandom(hello + HELLO_NONCE, NONCE_SIZE) < 0)
      return -1;
  }
  put32(hello + HELLO_MAXSIZE, conn->MaxSize);
  for (int i = 0; i < 256; i++) {
    struct SXDataType *dt = conn->SXDataType[i];
    hello[HELLO_SIZEBYTES + i] =
        NULL != dt ? (uint8_t)dt->SizeBytes : UNDEFINED_TYPE;
    put32(hello + HELLO_FIXEDSIZE + 4 * i, NULL != dt ? dt->FixedSize : 0);
  }
  return 0;
}

// The handshake. server tells which side we are: the two directions have
// different keys. -5: the key confirmation failed.
static int connectionInit(int sock, struct SXConnection *conn, int server) {
  uint8_t hello[HELLO_SIZE], peer[HELLO_SIZE];
  if (prepareHello(hello, conn) < 0)
    return -1;
  if (loopWrite(sock, hello, sizeof(hello),
                handshakeDeadline(&conn->WriteTimeout)) < 0)
    return -1;
  if (recvAll(sock, peer, sizeof(peer),
              handshakeDeadline(&conn->ReadTimeout)) < 0)
    return -2;
  // everything but the nonce has to be equal
  if (0 != memcmp(hello, peer, HELLO_NONCE) ||
      0 != memcmp(hello + HELLO_MAXSIZE, peer + HELLO_MAXSIZE,
                  HELLO_SIZE - HELLO_MAXSIZE))
    return -3;
  int rc = encSetup(sock, conn, server ? peer : hello, server ? hello : peer,
                    server);
  return 0 == rc ? 0 : -2 == rc ? -5 : -4;
}

// Nothing to check: the hello is built byte by byte.
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

  int rc = connectionInit(sock, conn, 0);
  if (rc < 0) {
    close(sock);
    return -5 == rc ? -9 : -6;
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

  int rc = connectionInit(sock, conn, 1);
  if (rc < 0) {
    close(sock);
    return -5 == rc ? -9 : -6;
  }

  return sock;
}

int SXClose(int sock) {
  encReset(sock);
  return close(sock);
}

// 0, or the SXWriteMsg code for a message of len bytes that does not fit the
// type.
static int checkSize(const struct SXDataType *dt, uint32_t len) {
  switch (dt->SizeBytes) {
  case -1:
    return dt->FixedSize == len ? 0 : -2;
  case 0:
    return 0 == len ? 0 : -2;
  case 1:
  case 2:
  case 3:
    return 0 == len >> (8 * dt->SizeBytes) ? 0 : -3;
  case 4: // MaxSize is the limit
    return 0;
  default:
    return -4;
  }
}

int SXWriteMsg(struct SXConnection *conn, int sock, uint8_t msgType,
               const void *msg, uint32_t msgLen) {

  struct SXDataType *dt = conn->SXDataType[msgType];

  if (NULL == dt) {
    return -1;
  }

  int rc = checkSize(dt, msgLen);
  if (rc < 0) {
    return rc;
  }

  // the peer has the same MaxSize and would drop the connection; a record
  // counts the type byte into its 4-byte length
  if (msgLen > conn->MaxSize || UINT32_MAX == msgLen) {
    return -3;
  }

  // One buffer and one send per frame: with TCP_NODELAY the parts sent
  // separately are separate TCP segments. 4 + 1 + TAG_SIZE is the longer of
  // the two kinds of frame.
  uint8_t *frame = malloc((size_t)msgLen + 4 + 1 + TAG_SIZE);
  if (NULL == frame) {
    return -10;
  }

  size_t len = 0;
  if (encActive(conn, sock)) {
    // length, then data and type encrypted in place, then the tag
    put32(frame, msgLen + 1);
    if (msgLen > 0) {
      memcpy(frame + 4, msg, msgLen);
    }
    frame[4 + (size_t)msgLen] = msgType;
    len = 4 + (size_t)msgLen + 1;
    if (encRecord(sock, 1, frame, 4, frame + 4, len - 4, frame + len) < 0) {
      free(frame);
      return -5;
    }
    len += TAG_SIZE;
  } else {
    // type, size, data, crc
    frame[len++] = msgType;
    for (int i = 0; i < dt->SizeBytes; i++) {
      frame[len++] = (uint8_t)(msgLen >> (8 * i));
    }
    if (msgLen > 0) {
      memcpy(frame + len, msg, msgLen);
      put32(frame + len + msgLen, sxCrc32(msg, msgLen));
      len += (size_t)msgLen + 4;
    }
  }

  int result = loopWrite(sock, frame, len, deadlineAfter(&conn->WriteTimeout));
  free(frame);
  return result;
}

// Reads size bytes and extra more into a new buffer; the caller frees *buf.
static int readBody(struct SXConnection *conn, int sock, uint32_t size,
                    uint32_t extra, uint8_t **buf) {
  uint64_t total = (uint64_t)size + extra;
  if (total != (size_t)total) { // a 32-bit size_t
    return -10;
  }
  *buf = malloc((size_t)total);
  if (NULL == *buf) {
    return -10;
  }
  return recvAll(sock, *buf, (size_t)total,
                 deadlineAfter(&conn->ReadTimeout)) < 0
             ? -2
             : 0;
}

// One message of a connection without a key: type, size, data, crc.
// *buf is allocated here and freed by the caller, also after an error.
static int readFrame(struct SXConnection *conn, int sock,
                     struct SXDataType **dtOut, uint8_t **buf,
                     uint32_t *sizeOut) {
  uint8_t hdr[5];
  if (recvAll(sock, hdr, 1, deadlineAfter(&conn->ReadTimeout)) < 0) {
    return -2;
  }

  // SizeBytes is local config, but a value above 4 would let the peer write
  // past hdr.
  struct SXDataType *dt = conn->SXDataType[hdr[0]];
  if (NULL == dt || dt->SizeBytes < -1 || dt->SizeBytes > 4) {
    return -1;
  }

  uint32_t size = -1 == dt->SizeBytes ? dt->FixedSize : 0;
  if (dt->SizeBytes > 0) {
    if (recvAll(sock, hdr + 1, (size_t)dt->SizeBytes,
                deadlineAfter(&conn->ReadTimeout)) < 0) {
      return -2;
    }
    for (int i = 0; i < dt->SizeBytes; i++) {
      size |= (uint32_t)hdr[1 + i] << (8 * i);
    }
  }

  // MaxSize bounds what a peer can make us allocate per message.
  if (size > conn->MaxSize) {
    return -4;
  }

  *dtOut = dt;
  *sizeOut = size;
  if (0 == size) {
    return 0;
  }

  int rc = readBody(conn, sock, size, 4, buf); // data and crc
  if (rc < 0) {
    return rc;
  }
  return get32(*buf + size) == sxCrc32(*buf, size) ? 0 : -3;
}

// One message of a connection with a key: the length, then data and type
// encrypted, then the tag. *buf as in readFrame.
static int readRecord(struct SXConnection *conn, int sock,
                      struct SXDataType **dtOut, uint8_t **buf,
                      uint32_t *sizeOut) {
  uint8_t lenBuf[4];
  if (recvAll(sock, lenBuf, sizeof(lenBuf),
              deadlineAfter(&conn->ReadTimeout)) < 0) {
    return -2;
  }

  // Checked before the tag can be: MaxSize bounds what anybody in the path
  // can make us allocate.
  uint32_t len = get32(lenBuf); // data and type
  if (0 == len || len - 1 > conn->MaxSize) {
    return -4;
  }

  int rc = readBody(conn, sock, len, TAG_SIZE, buf);
  if (rc < 0) {
    return rc;
  }
  if (encRecord(sock, 0, lenBuf, sizeof(lenBuf), *buf, len, *buf + len) < 0) {
    return -3;
  }

  struct SXDataType *dt = conn->SXDataType[(*buf)[len - 1]];
  if (NULL == dt) {
    return -1;
  }
  if (checkSize(dt, len - 1) < 0) {
    return -4;
  }
  *dtOut = dt;
  *sizeOut = len - 1;
  return 0;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {
  struct SXDataType *dt = NULL;
  uint8_t *buf = NULL;
  uint32_t size = 0;

  int rc = encActive(conn, sock) ? readRecord(conn, sock, &dt, &buf, &size)
                                 : readFrame(conn, sock, &dt, &buf, &size);
  if (0 == rc && dt->Callback) {
    dt->Callback(size > 0 ? buf : NULL, size, sock, conn);
  }

  free(buf);
  return rc;
}
