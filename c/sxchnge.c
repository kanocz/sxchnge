#include <netdb.h>
#include <netinet/tcp.h>
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

inline static int loopWrite(int sock, uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    ssize_t x = write(sock, data + i, len - i);
    if (x < 0) {
      return -5;
    }
    i += (size_t)x;
  }
  return 0;
}

static void prepareInitialPacket(struct initialPacket *ip,
                                 struct SXConnection *conn) {
  static char header[8] = headerHello;
  for (int i = 0; i < 8; i++)
    ip->Header[i] = header[i];
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

  if (loopWrite(sock, header, headerLen) < 0)
    return -5;
  if (loopWrite(sock, (uint8_t *)msg, msgLen) < 0)
    return -5;
  if (msgLen > 0) {
    u_int32_t crc = crc32c((unsigned char *)msg, msgLen);
    if (loopWrite(sock, (uint8_t *)(&crc), 4) < 0)
      return -5;
  }

  return 0;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {

  uint8_t msgType = 0;

  if (recv(sock, &msgType, 1, MSG_WAITALL) != 1) {
    return -2;
  }

  struct SXDataType *dt = conn->SXDataType[msgType];

  if (NULL == dt) {
    return -1;
  }

  uint8_t sizebuf[4];
  if (dt->SizeBytes > 0) {
    if (recv(sock, sizebuf, (size_t)dt->SizeBytes, MSG_WAITALL) !=
        dt->SizeBytes) {
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

  uint8_t *buf = NULL;
  if (size2read > 0) {
    buf = malloc(size2read+4);
    if (NULL == buf) {
      return -10;
    }
    if (recv(sock, buf, size2read+4, MSG_WAITALL) != size2read+4) {
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
