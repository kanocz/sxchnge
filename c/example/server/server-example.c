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

void stringHandler(void *data, size_t size, int sock,
                   struct SXConnection *conn) {
  (void)(sock); // unused param
  (void)(conn); // unused param

  printf("String received: %.*s\n", (int)size, (char *)data);
}

void someDataHandler(void *data, size_t size, int sock,
                     struct SXConnection *conn) {
  (void)(sock); // unused param
  (void)(conn); // unused param
  (void)(size); // unused param

  someData *value = (someData *)data;

  printf("SomeData received: ID: %ld, Value: %f\n", value->ID, value->Value);
}

void jsonHandler(void *data, size_t size, int sock, struct SXConnection *conn) {
  (void)(sock); // unused param
  (void)(conn); // unused param

  // We don't decode JSON in this example :)

  printf("JSON received: %.*s\n", (int)size, (char *)data);
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
              &(struct SXDataType){// typeSting
                                   .SizeBytes = 1,
                                   .Callback = stringHandler},
              &(struct SXDataType){// typeSomeData
                                   .SizeBytes = -1,
                                   .FixedSize = sizeof(someData),
                                   .Callback = someDataHandler},
              &(struct SXDataType){// typeSomeDataJson
                                   .SizeBytes = 2,
                                   .Callback = jsonHandler},
          }};

  if (0 != SXListen(3456, &sxconn)) {
    perror("Listen error");
    return -1;
  }

  int sock = SXAccpet(&sxconn);
  if (sock < 0) {
    printf("Accept returned %d: ", sock);
    fflush(stdout);
    perror("");
    return -1;
  }

  puts("We have connection, sending ping...");
  if (0 != SXWriteMsg(&sxconn, sock, typePing, NULL, 0)) {
    perror("SXWriteMsg");
    return -2;
  }

  while (1) {
    puts("Processing incomming message...");
    if (0 != SXProcessMsg(&sxconn, sock)) {
      perror("SXProcessMsg");
      return -3;
    }
  }
}
