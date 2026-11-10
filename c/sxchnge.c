#include <ctype.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

inline static int setSockOptions(int sock, struct SXConnection *conn) {
  int yes = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(int)) < 0) {
    close(sock);
    return -5;
  }

  if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &conn->KeepAlive,
                 sizeof(int)) < 0) {
    close(sock);
    return -5;
  }

  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &conn->ReadTimeout,
                 sizeof(conn->ReadTimeout)) < 0) {
    close(sock);
    return -5;
  }
  if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &conn->WriteTimeout,
                 sizeof(conn->ReadTimeout)) < 0) {
    close(sock);
    return -5;
  }
  return 0;
}

inline static int loopWrite(int sock, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len;) {
    ssize_t x = write(sock, data + i, len - i);
    if (x < 0) {
      return -5;
    }
    i += (size_t)x;
  }
  return 0;
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
      recv(sock, peerIV, sizeof(peerIV), MSG_WAITALL) == sizeof(peerIV)) {
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

static int sxWrite(int sock, const uint8_t *data, size_t len) {
  EVP_CIPHER_CTX *ctx = encFd(sock) ? ciphers[sock].w : NULL;
  if (NULL == ctx)
    return loopWrite(sock, data, len);

  uint8_t buf[4096]; // data belongs to the caller, so encrypt into a copy
  while (len > 0) {
    int n = len < sizeof(buf) ? (int)len : (int)sizeof(buf);
    int out;
    if (EVP_EncryptUpdate(ctx, buf, &out, data, n) != 1 ||
        loopWrite(sock, buf, (size_t)out) < 0)
      return -5;
    data += n;
    len -= (size_t)n;
  }
  return 0;
}

static int sxRead(int sock, uint8_t *buf, size_t len) {
  if (recv(sock, buf, len, MSG_WAITALL) != (ssize_t)len)
    return -1;
  EVP_CIPHER_CTX *ctx = encFd(sock) ? ciphers[sock].r : NULL;
  int out;
  // CTR decryption is the same keystream XOR as encryption
  if (NULL != ctx && EVP_EncryptUpdate(ctx, buf, &out, buf, (int)len) != 1)
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
  if (recv(sock, recvBuf, initialPacketSize, MSG_WAITALL) != initialPacketSize)
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

int SXConnect(char *ip, uint16_t port, struct SXConnection *conn) {

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == sock) {
    return -1;
  }

  struct sockaddr_in sockaddrin;
  bzero(&sockaddrin, sizeof(sockaddrin));
  sockaddrin.sin_family = AF_INET;
  sockaddrin.sin_port = htons(port);

  struct hostent *host;
  host = gethostbyname(ip);
  if (NULL == host) {
    return -2;
  }
  if (1 > host->h_length) {
    return -3;
  }

  bcopy(host->h_addr_list[0], &sockaddrin.sin_addr, (size_t)host->h_length);

  if (-1 == connect(sock, (struct sockaddr *)&sockaddrin,
                    sizeof(struct sockaddr_in))) {
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

int SXListen(uint16_t port, struct SXConnection *conn) {

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == sock) {
    return -1;
  }

  int reuseaddrVal = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddrVal,
                 sizeof(reuseaddrVal)) == -1) {
    return -2;
  }

  struct sockaddr_in sockaddrin;
  bzero(&sockaddrin, sizeof(sockaddrin));
  sockaddrin.sin_family = AF_INET;
  sockaddrin.sin_port = htons(port);
  sockaddrin.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&sockaddrin, sizeof(sockaddrin)) < 0) {
    return -3;
  }

  if (listen(sock, LISTEN_BACKLOG) < 0) {
    return -4;
  }

  conn->socket = sock;

  return 0;
}

int SXAccpet(struct SXConnection *conn) {
  struct sockaddr_in clientaddr;
  unsigned int clientaddrlen = sizeof(clientaddr);

  int sock =
      accept(conn->socket, (struct sockaddr *)&clientaddr, &clientaddrlen);
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
    if (msgLen > 255 * 255) {
      return -3;
    }
    header[1] = (uint8_t)(msgLen & 0xff);
    header[2] = (uint8_t)((msgLen & 0xff00) >> 8);
    headerLen = 3;
    break;
  case 3:;
    if (msgLen > 255 * 255) {
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

  if (sxWrite(sock, header, headerLen) < 0)
    return -5;
  if (sxWrite(sock, (uint8_t *)msg, msgLen) < 0)
    return -5;
  if (msgLen > 0) {
    u_int32_t crc = crc32c((unsigned char *)msg, msgLen);
    if (sxWrite(sock, (uint8_t *)(&crc), 4) < 0)
      return -5;
  }

  return 0;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {

  uint8_t msgType = 0;

  if (sxRead(sock, &msgType, 1) < 0) {
    return -2;
  }

  struct SXDataType *dt = conn->SXDataType[msgType];

  if (NULL == dt) {
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
    buf = malloc(size2read+4);
    if (NULL == buf) {
      return -10;
    }
    if (sxRead(sock, buf, size2read+4) < 0) {
      free(buf);
      return -2;
    }
    if ((*(u_int32_t*)(&buf[size2read])) != crc32c((unsigned char *)buf, size2read)) {
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
