#define _DEFAULT_SOURCE // getaddrinfo and friends under -std=c11 on glibc

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdio.h>

#include "crc32.h"
#include "sxchnge.h"

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
inline static int setSockOptions(int sock, struct SXConnection *conn) {
  int yes = 1;
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

  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &conn->ReadTimeout,
                 sizeof(conn->ReadTimeout)) < 0) {
    return -5;
  }
  if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &conn->WriteTimeout,
                 sizeof(conn->WriteTimeout)) < 0) {
    return -5;
  }
  return 0;
}

inline static int loopWrite(int sock, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len;) {
    ssize_t x = send(sock, data + i, len - i, MSG_NOSIGNAL);
    if (x < 0) {
      if (EINTR == errno)
        continue;
      return -5;
    }
    i += (size_t)x;
  }
  return 0;
}

// Reads exactly len bytes. A short read (timeout, closed connection, signal
// in the middle of the data) is an error: with MSG_WAITALL the read timeout
// covers the whole buffer, so a retry would let a slow peer stretch it.
inline static int recvAll(int sock, void *buf, size_t len) {
  ssize_t n;
  do {
    n = recv(sock, buf, len, MSG_WAITALL);
  } while (n < 0 && EINTR == errno); // nothing was read yet
  return n == (ssize_t)len ? 0 : -1;
}

// AES-CTR state of one socket, one context per direction. One SXConnection
// serves every socket returned by SXAccpet, so the state is keyed by fd.
// ponytail: fixed table, an encrypted socket needs fd < SX_MAX_ENC_FD; replace
// with a growable map if a process holds more descriptors than that.
#define SX_MAX_ENC_FD 65536
static struct {
  EVP_CIPHER_CTX *r, *w;
} ciphers[SX_MAX_ENC_FD];

inline static int encEnabled(struct SXConnection *conn) {
  return NULL != conn->AESKey && '\0' != conn->AESKey[0];
}

inline static int encFd(int sock) { return sock >= 0 && sock < SX_MAX_ENC_FD; }

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
      loopWrite(sock, iv, sizeof(iv)) == 0 &&
      recvAll(sock, peerIV, sizeof(peerIV)) == 0) {
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

// Encrypts buf in place when the socket has a cipher, then sends it.
static int sxWrite(int sock, uint8_t *buf, size_t len) {
  EVP_CIPHER_CTX *ctx = encFd(sock) ? ciphers[sock].w : NULL;
  int out;
  if (NULL != ctx && (len > INT_MAX ||
                      EVP_EncryptUpdate(ctx, buf, &out, buf, (int)len) != 1))
    return -5;
  return loopWrite(sock, buf, len);
}

static int sxRead(int sock, uint8_t *buf, size_t len) {
  if (recvAll(sock, buf, len) < 0)
    return -1;
  EVP_CIPHER_CTX *ctx = encFd(sock) ? ciphers[sock].r : NULL;
  int out;
  // CTR decryption is the same keystream XOR as encryption
  if (NULL != ctx && (len > INT_MAX ||
                      EVP_EncryptUpdate(ctx, buf, &out, buf, (int)len) != 1))
    return -1;
  return 0;
}

static void prepareInitialPacket(struct initialPacket *ip,
                                 struct SXConnection *conn) {
  static char header[8] = headerHello;
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
  char sendBuf[initialPacketSize];
  char recvBuf[initialPacketSize];
  prepareInitialPacket((struct initialPacket *)sendBuf, conn);
  if (loopWrite(sock, (uint8_t *)sendBuf, initialPacketSize) < 0)
    return -1;
  if (recvAll(sock, recvBuf, initialPacketSize) < 0)
    return -2;
  if (0 != memcmp(sendBuf, recvBuf, initialPacketSize))
    return -3;
  if (encSetup(sock, conn) < 0)
    return -4;
  return 0;
}

int SXInit(void) {
  if (sizeof(struct initialPacket) != initialPacketSize) {
    printf("Comiler aligment not equeal to golang style, please check (%ld != "
           "%d)\n",
           sizeof(struct initialPacket), initialPacketSize);
    return -1;
  }
  return 0;
}

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

// connect() that gives up after tv; a zero tv keeps the system default.
static int connectTimeout(int sock, const struct sockaddr *addr,
                          socklen_t addrlen, const struct timeval *tv) {
  if (0 == tv->tv_sec && 0 == tv->tv_usec)
    return connect(sock, addr, addrlen);

  int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    return -1;

  int rc = connect(sock, addr, addrlen);
  if (rc < 0 && EINPROGRESS == errno) {
    struct pollfd pfd = {.fd = sock, .events = POLLOUT};
    int ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    int err = 0;
    socklen_t errlen = sizeof(err);
    rc = -1;
    if (1 == poll(&pfd, 1, ms) &&
        0 == getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) && 0 == err)
      rc = 0;
  }

  if (fcntl(sock, F_SETFL, flags) < 0)
    return -1;
  return rc;
}

int SXConnect(const char *ip, uint16_t port, struct SXConnection *conn) {

  struct addrinfo *res = resolve(ip, port, 0);
  if (NULL == res) {
    return -2;
  }

  int sock = -1;
  for (struct addrinfo *ai = res; NULL != ai; ai = ai->ai_next) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) {
      continue;
    }
    if (0 == connectTimeout(sock, ai->ai_addr, ai->ai_addrlen,
                            &conn->ConnectTimeout)) {
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

  struct addrinfo *res = resolve(ip, port, AI_PASSIVE);
  if (NULL == res) {
    return -3;
  }

  int result = 0;
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  int reuseaddrVal = 1;
  if (-1 == sock) {
    result = -1;
  } else if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddrVal,
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

int SXWriteMsg(struct SXConnection *conn, int sock, uint8_t msgType, char *msg,
               uint32_t msgLen) {

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
  case 3:;
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

  // One buffer and one send per frame: header, body and crc sent separately
  // wait for each other's ACK (Nagle against delayed ACK, 40 ms on Linux).
  size_t frameLen = headerLen + (size_t)msgLen + (msgLen > 0 ? 4 : 0);
  uint8_t *frame = malloc(frameLen);
  if (NULL == frame) {
    return -10;
  }
  memcpy(frame, header, headerLen);
  if (msgLen > 0) {
    u_int32_t crc = crc32c((unsigned char *)msg, msgLen);
    memcpy(frame + headerLen, msg, msgLen);
    memcpy(frame + headerLen + msgLen, &crc, sizeof(crc));
  }

  int result = sxWrite(sock, frame, frameLen);
  free(frame);
  return result;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {

  uint8_t msgType = 0;

  if (sxRead(sock, &msgType, 1) < 0) {
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
    if (sxRead(sock, sizebuf, (size_t)dt->SizeBytes) < 0) {
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
    if (sxRead(sock, buf, total) < 0) {
      free(buf);
      return -2;
    }
    u_int32_t crc; // the crc is not aligned inside buf
    memcpy(&crc, buf + size2read, sizeof(crc));
    if (crc != crc32c((unsigned char *)buf, size2read)) {
      free(buf);
      return -3;
    }
  }

  if (dt->Callback) {
    dt->Callback(buf, size2read, sock, (struct SXConnection *)conn);
  }

  if (buf) {
    free(buf);
  }

  return 0;
}
