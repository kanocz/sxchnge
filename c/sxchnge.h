#ifndef __SXCHNGE_H__
#define __SXCHNGE_H__

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

struct SXConnection;

typedef void (*SXCB)(void *data, size_t size, int sock, struct SXConnection *conn);

struct SXDataType {
  int SizeBytes;
  uint32_t FixedSize;
  SXCB Callback;
};

struct SXConnection {
  int socket;
  // will we use it in multi-thread? add mutex?
  struct SXDataType *SXDataType[256];
  int KeepAlive;
  struct timeval ReadTimeout;
  struct timeval WriteTimeout;
  int MaxSize;
} ;

int SXConnect(char *ip, uint16_t port, struct SXConnection *conn);
int SXListen(uint16_t port, struct SXConnection *conn);
int SXAccpet(struct SXConnection *conn);
int SXWriteMsg(struct SXConnection *conn, int sock, uint8_t msgType, char *msg,
               uint32_t msgLen);
int SXProcessMsg(struct SXConnection *conn, int sock);

#endif
