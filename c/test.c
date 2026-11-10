// Tests of the C library: a server in the main thread and a client in a second
// thread talk over loopback. Run with "make test".
#define _DEFAULT_SOURCE

#include "sxchnge.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

enum { tEmpty, tOne, tTwo, tThree, tFixed, tCount, tBad = 9 };
#define FIXED 16
#define MAXSIZE 100000
#define KEY128 "00112233445566778899AABBCCDDEEFF"
#define KEY192 KEY128 "0011223344556677"
#define KEY256 KEY128 KEY128

static uint8_t pattern[70000];

// the last message seen by the server and by the client
struct seen {
  int count;
  size_t size;
  uint8_t data[sizeof(pattern)];
};
static struct seen srvSeen, cliSeen;

static void record(struct seen *s, void *data, size_t size) {
  s->count++;
  s->size = size;
  if (size > 0 && size <= sizeof(s->data))
    memcpy(s->data, data, size);
}

static void srvCB(void *data, size_t size, int sock, struct SXConnection *c) {
  (void)sock, (void)c;
  record(&srvSeen, data, size);
}

static void cliCB(void *data, size_t size, int sock, struct SXConnection *c) {
  (void)sock, (void)c;
  record(&cliSeen, data, size);
}

static struct SXDataType srvTypes[tCount] = {
    {0, 0, srvCB}, {1, 0, srvCB}, {2, 0, srvCB}, {3, 0, srvCB},
    {-1, FIXED, srvCB}};
static struct SXDataType cliTypes[tCount] = {
    {0, 0, cliCB}, {1, 0, cliCB}, {2, 0, cliCB}, {3, 0, cliCB},
    {-1, FIXED, cliCB}};
static struct SXDataType badType = {5, 0, NULL};        // invalid SizeBytes
static struct SXDataType otherSizeType = {2, 0, NULL};  // tOne with SizeBytes 2

static struct SXConnection mkConn(struct SXDataType *types, const char *key) {
  struct SXConnection c = {.KeepAlive = 5,
                           .MaxSize = MAXSIZE,
                           .ReadTimeout = {.tv_sec = 2},
                           .WriteTimeout = {.tv_sec = 2},
                           .ConnectTimeout = {.tv_sec = 2},
                           .AESKey = key};
  for (int i = 0; i < tCount; i++)
    c.SXDataType[i] = &types[i];
  return c;
}

struct client {
  struct SXConnection conn;
  uint16_t port;
  int (*fn)(struct SXConnection *, int); // what to do once connected
  int connectRc, rc;
  pthread_t thread;
};

static void *clientMain(void *arg) {
  struct client *cl = arg;
  cl->rc = -1000;
  cl->connectRc = SXConnect("127.0.0.1", cl->port, &cl->conn);
  if (0 == cl->connectRc) {
    cl->rc = cl->fn(&cl->conn, cl->conn.socket);
    SXClose(cl->conn.socket);
  }
  return NULL;
}

static uint16_t listenPort(struct SXConnection *srv) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  CHECK(0 == getsockname(srv->socket, (struct sockaddr *)&addr, &len));
  CHECK(htonl(INADDR_LOOPBACK) == addr.sin_addr.s_addr);
  return ntohs(addr.sin_port);
}

// Listens on loopback, starts the client and returns what SXAccpet returned.
static int start(struct SXConnection *srv, struct client *cl) {
  CHECK(0 == SXListen("127.0.0.1", 0, srv));
  cl->port = listenPort(srv);
  CHECK(0 == pthread_create(&cl->thread, NULL, clientMain, cl));
  return SXAccpet(srv);
}

static void finish(struct SXConnection *srv, struct client *cl, int sock) {
  pthread_join(cl->thread, NULL);
  if (sock >= 0)
    SXClose(sock);
  SXClose(srv->socket);
}

// --- every kind of type in both directions, plain and encrypted

static const struct {
  uint8_t type;
  uint32_t size;
} roundTripMsgs[] = {{tEmpty, 0},     {tOne, 0},        {tOne, 255},
                     {tTwo, 0xffff},  {tThree, 70000},  {tFixed, FIXED}};
#define ROUNDTRIP_COUNT (int)(sizeof(roundTripMsgs) / sizeof(roundTripMsgs[0]))

static int roundTripClient(struct SXConnection *conn, int sock) {
  for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
    int rc = SXWriteMsg(conn, sock, roundTripMsgs[i].type, (char *)pattern,
                        roundTripMsgs[i].size);
    if (0 != rc)
      return rc;
  }
  return SXProcessMsg(conn, sock); // the reply of the server
}

static void testRoundTrip(const char *key) {
  struct SXConnection srv = mkConn(srvTypes, key);
  struct client cl = {.conn = mkConn(cliTypes, key), .fn = roundTripClient};
  memset(&srvSeen, 0, sizeof(srvSeen));
  memset(&cliSeen, 0, sizeof(cliSeen));

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  for (int i = 0; sock >= 0 && i < ROUNDTRIP_COUNT; i++) {
    CHECK(0 == SXProcessMsg(&srv, sock));
    CHECK(srvSeen.count == i + 1);
    CHECK(srvSeen.size == roundTripMsgs[i].size);
    CHECK(0 == memcmp(srvSeen.data, pattern, srvSeen.size));
  }
  if (sock >= 0)
    CHECK(0 == SXWriteMsg(&srv, sock, tOne, "reply", 5));
  finish(&srv, &cl, sock);

  CHECK(0 == cl.connectRc);
  CHECK(0 == cl.rc);
  CHECK(1 == cliSeen.count && 5 == cliSeen.size);
  CHECK(0 == memcmp(cliSeen.data, "reply", 5));
}

// --- handshake refuses a peer with another configuration

static int noop(struct SXConnection *conn, int sock) {
  (void)conn, (void)sock;
  return 0;
}

static void expectHandshakeFailure(struct SXConnection srv,
                                   struct SXConnection cli) {
  struct client cl = {.conn = cli, .fn = noop};
  int sock = start(&srv, &cl);
  CHECK(-6 == sock);
  finish(&srv, &cl, sock);
  CHECK(-6 == cl.connectRc);
}

static void testHandshakeMismatch(void) {
  struct SXConnection cli = mkConn(cliTypes, KEY128);
  cli.SXDataType[tOne] = &otherSizeType;
  expectHandshakeFailure(mkConn(srvTypes, KEY128), cli); // SizeBytes

  cli = mkConn(cliTypes, KEY128);
  cli.MaxSize++;
  expectHandshakeFailure(mkConn(srvTypes, KEY128), cli); // MaxSize

  expectHandshakeFailure(mkConn(srvTypes, KEY128),
                         mkConn(cliTypes, NULL)); // encryption on one side

  expectHandshakeFailure(mkConn(srvTypes, "zz"),
                         mkConn(cliTypes, "zz")); // not a key
  expectHandshakeFailure(mkConn(srvTypes, KEY128 "00"),
                         mkConn(cliTypes, KEY128 "00")); // 17 bytes
}

// --- frames the server has to refuse

static const uint8_t *rawFrame;
static size_t rawFrameLen;

static int rawClient(struct SXConnection *conn, int sock) {
  (void)conn;
  return send(sock, rawFrame, rawFrameLen, 0) == (ssize_t)rawFrameLen ? 0 : -1;
}

static void expectRefusal(const uint8_t *frame, size_t len, int want) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = rawClient};
  srv.SXDataType[tBad] = cl.conn.SXDataType[tBad] = &badType;
  rawFrame = frame;
  rawFrameLen = len;

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0) {
    int rc = SXProcessMsg(&srv, sock);
    if (rc != want)
      printf("SXProcessMsg returned %d, want %d\n", rc, want);
    CHECK(rc == want);
  }
  finish(&srv, &cl, sock);
}

static void testRefusedFrames(void) {
  const uint8_t unknownType[] = {200};
  const uint8_t invalidSizeBytes[] = {tBad, 1, 2, 3, 4, 5, 6, 7, 8};
  const uint8_t badCrc[] = {tOne, 2, 'h', 'i', 0, 0, 0, 0};
  const uint8_t overMaxSize[] = {tThree, 0xff, 0xff, 0xff};
  const uint8_t truncated[] = {tOne, 5, 'h'};

  expectRefusal(unknownType, sizeof(unknownType), -1);
  expectRefusal(invalidSizeBytes, sizeof(invalidSizeBytes), -1);
  expectRefusal(badCrc, sizeof(badCrc), -3);
  expectRefusal(overMaxSize, sizeof(overMaxSize), -4);
  expectRefusal(truncated, sizeof(truncated), -2);
}

// --- a message encrypted with another key is not accepted

static struct SXDataType onlySized = {2, 0, NULL};

static int sizedClient(struct SXConnection *conn, int sock) {
  return SXWriteMsg(conn, sock, 0, (char *)pattern, 100);
}

static void testWrongKey(void) {
  // only a sized type: a zero-size one would pass for 1 of 256 garbage bytes
  struct SXConnection srv = {.KeepAlive = 5,
                             .MaxSize = MAXSIZE,
                             .ReadTimeout = {.tv_usec = 300000},
                             .WriteTimeout = {.tv_sec = 2},
                             .AESKey = KEY128,
                             .SXDataType = {&onlySized}};
  struct client cl = {.conn = srv, .fn = sizedClient};
  cl.conn.AESKey = "01112233445566778899AABBCCDDEEFF";

  int sock = start(&srv, &cl); // the handshake does not check the key
  CHECK(sock >= 0);
  if (sock >= 0)
    CHECK(0 != SXProcessMsg(&srv, sock));
  finish(&srv, &cl, sock);
}

// --- SXWriteMsg refuses what the peer could not read; nothing is sent

static void testWriteLimits(void) {
  struct SXConnection conn = mkConn(srvTypes, NULL);
  conn.SXDataType[tBad] = &badType;
  char *buf = (char *)pattern;

  CHECK(-1 == SXWriteMsg(&conn, -1, 200, buf, 1));           // undefined type
  CHECK(-2 == SXWriteMsg(&conn, -1, tFixed, buf, FIXED - 1)); // not FixedSize
  CHECK(-3 == SXWriteMsg(&conn, -1, tOne, buf, 256));
  CHECK(-3 == SXWriteMsg(&conn, -1, tTwo, buf, 0x10000));
  CHECK(-3 == SXWriteMsg(&conn, -1, tThree, buf, MAXSIZE + 1));
  CHECK(-4 == SXWriteMsg(&conn, -1, tBad, buf, 1));
}

// --- writing to a connection closed by the peer is an error, not SIGPIPE

static void testWriteToClosedPeer(void) {
  struct SXConnection srv = mkConn(srvTypes, KEY128);
  struct client cl = {.conn = mkConn(cliTypes, KEY128), .fn = noop};

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  pthread_join(cl.thread, NULL); // the client has closed its socket

  // keep writing after the first error: the signal comes with a later write
  int errors = 0;
  for (int i = 0; sock >= 0 && errors < 3 && i < 200; i++) {
    if (-5 == SXWriteMsg(&srv, sock, tOne, "ping", 4))
      errors++;
    usleep(10000);
  }
  CHECK(3 == errors);

  if (sock >= 0)
    SXClose(sock);
  SXClose(srv.socket);
}

// --- a silent peer ends SXProcessMsg after ReadTimeout

static int silentClient(struct SXConnection *conn, int sock) {
  (void)conn, (void)sock;
  usleep(600000);
  return 0;
}

static void testReadTimeout(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  srv.ReadTimeout = (struct timeval){.tv_usec = 200000};
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = silentClient};

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0)
    CHECK(-2 == SXProcessMsg(&srv, sock));
  finish(&srv, &cl, sock);
}

// --- listen and connect failures

static void testListenAndConnectErrors(void) {
  struct SXConnection first = mkConn(srvTypes, NULL);
  struct SXConnection second = mkConn(srvTypes, NULL);
  CHECK(0 == SXListen("127.0.0.1", 0, &first));
  uint16_t port = listenPort(&first);

  CHECK(-3 == SXListen("127.0.0.1", port, &second)); // the port is taken
  SXClose(first.socket);

  CHECK(-4 == SXConnect("127.0.0.1", port, &second)); // nobody listens

  CHECK(0 == SXListen(NULL, 0, &first)); // every interface
  SXClose(first.socket);
}

int main(void) {
  // a parent that ignores SIGPIPE would hide a missing protection
  signal(SIGPIPE, SIG_DFL);

  for (size_t i = 0; i < sizeof(pattern); i++)
    pattern[i] = (uint8_t)(i * 7);

  CHECK(0 == SXInit());
  testRoundTrip(NULL);
  testRoundTrip(KEY128);
  testRoundTrip(KEY192);
  testRoundTrip(KEY256);
  testHandshakeMismatch();
  testRefusedFrames();
  testWrongKey();
  testWriteLimits();
  testWriteToClosedPeer();
  testReadTimeout();
  testListenAndConnectErrors();

  if (failures > 0) {
    printf("%d checks FAILED\n", failures);
    return 1;
  }
  puts("C tests OK");
  return 0;
}
