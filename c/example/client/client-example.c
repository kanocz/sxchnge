#include "../../sxchnge.h"
#include <stdio.h>

enum dataType {
  typePing = 0,
  typePong,
  typeString,
  typeSomeData,
  typeSomeDataJSON
};

typedef struct {
  uint64_t ID;
  double Value; // warning: in this example we assume that double has 64 bits!
} someData;

void pingHandler(void *data, size_t size, int sock, struct SXConnection *conn) {

  (void)(data); // unused param
  (void)(size); // unused param

  puts("Ping received, sending pong");
  SXWriteMsg(conn, sock, typePong, NULL, 0);
}

void pongHandler(void *data, size_t size, int sock, struct SXConnection *conn) {

  (void)(data); // unused param
  (void)(size); // unused param
  (void)(sock); // unused param
  (void)(conn); // unused param

  puts("Pong received");
}

int main() {

  struct SXConnection sxconn = {
      .KeepAlive = 5,
      .MaxSize = 4096,
      .ReadTimeout = {.tv_sec = 3, .tv_usec = 0},
      .WriteTimeout = {.tv_sec = 3, .tv_usec = 0},
      .SXDataType =
          {
              &(struct SXDataType){// typePing
                                   .SizeBytes = 0,
                                   .FixedSize = 0,
                                   .Callback = pingHandler},
              &(struct SXDataType){// typePong
                                   .SizeBytes = 0,
                                   .FixedSize = 0,
                                   .Callback = pongHandler},
              &(struct SXDataType){.SizeBytes = 1}, // send-only typeSting
              &(struct SXDataType){.SizeBytes = -1, // send-only typeSomeData
                                   .FixedSize = sizeof(someData)},
              &(struct SXDataType){.SizeBytes =
                                       2}, // send-only typeSomeDataJson
          }};

  if (0 != SXConnect("127.0.0.1", 3456, &sxconn)) {
    perror("Connection error");
    return -1;
  }

  puts("Processing incomming message... (waiting for pint from server)");
  if (0 != SXProcessMsg(&sxconn, sxconn.socket)) {
    perror("SXProcessMsg");
  }

  puts("Sending ping...");
  if (0 != SXWriteMsg(&sxconn, sxconn.socket, typePing, NULL, 0)) {
    perror("SXWriteMsg");
  }

  puts("Processing incomming message...");
  if (0 != SXProcessMsg(&sxconn, sxconn.socket)) {
    perror("SXProcessMsg");
  }

  puts("Sending string...");
  if (0 != SXWriteMsg(&sxconn, sxconn.socket, typeString, "Hello world!", 12)) {
    perror("SXWriteMsg");
  }

  someData someDataVal = {.ID = 333, .Value = 12.345678};

  puts("Sending someData...");
  if (0 != SXWriteMsg(&sxconn, sxconn.socket, typeSomeData,
                      (char *)&someDataVal, sizeof(someDataVal))) {
    perror("SXWriteMsg");
  }
}
