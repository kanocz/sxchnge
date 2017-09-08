#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sxchnge.h"

#define LISTEN_BACKLOG 1024

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

inline static int loopRead(int sock, uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    ssize_t x = read(sock, data + i, len - i);
    if (x < 0) {
      return -5;
    }
    i += (size_t)x;
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
    return -5;
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
    return -5;
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

  loopWrite(sock, header, headerLen);
  loopWrite(sock, (uint8_t *)msg, msgLen);

  return 0;
}

int SXProcessMsg(struct SXConnection *conn, int sock) {

  uint8_t msgType = 0;

  ssize_t x = 0;
  while (x == 0) {
    x = read(sock, &msgType, 1);
  }

  if (x < 0) {
    return -2;
  }

  struct SXDataType *dt = conn->SXDataType[msgType];

  if (NULL == dt) {
    return -1;
  }

  uint8_t sizebuf[4];
  loopRead(sock, sizebuf, (size_t)dt->SizeBytes);

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
    buf = malloc(size2read);
    if (NULL == buf) {
      return -10;
    }
    loopRead(sock, buf, size2read);
  }

  if (dt->Callback) {
    dt->Callback(buf, size2read, sock, (struct SXConnection *)conn);
  }

  if (buf) {
    free(buf);
  }

  return 0;
}
