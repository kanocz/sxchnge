// Tests of the C library: a server in the main thread and a client in a second
// thread talk over loopback. Run with "make -B test" and with
// "make -B ENCRYPTION=1 test": the two builds test different things.
#define _DEFAULT_SOURCE

#include "crc32.h"
#include "sxchnge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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

enum { tEmpty, tOne, tTwo, tThree, tFixed, tFour, tCount, tBad = 9 };
#define FIXED 16
#define MAXSIZE 100000
#define KEY128 "00112233445566778899AABBCCDDEEFF"
#define KEY192 KEY128 "0011223344556677"
#define KEY256 KEY128 KEY128

static uint8_t pattern[70000];
static uint8_t big[4 << 20]; // more than the socket buffers hold

static pthread_t mainThread; // the server side of every test
static void onSignal(int sig) { (void)sig; }

static double seconds(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

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
    {0, 0, srvCB}, {1, 0, srvCB},      {2, 0, srvCB},
    {3, 0, srvCB}, {-1, FIXED, srvCB}, {4, 0, srvCB}};
static struct SXDataType cliTypes[tCount] = {
    {0, 0, cliCB}, {1, 0, cliCB},      {2, 0, cliCB},
    {3, 0, cliCB}, {-1, FIXED, cliCB}, {4, 0, cliCB}};
static struct SXDataType badType = {5, 0, NULL};        // invalid SizeBytes
static struct SXDataType otherSizeType = {2, 0, NULL};  // tOne with SizeBytes 2
static struct SXDataType noDataType = {0, 0, NULL};

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
} roundTripMsgs[] = {{tEmpty, 0},     {tOne, 0},       {tOne, 255},
                     {tTwo, 0xffff},  {tThree, 70000}, {tFour, 70000},
                     {tFixed, FIXED}};
#define ROUNDTRIP_COUNT (int)(sizeof(roundTripMsgs) / sizeof(roundTripMsgs[0]))

static int roundTripClient(struct SXConnection *conn, int sock) {
  for (int i = 0; i < ROUNDTRIP_COUNT; i++) {
    int rc = SXWriteMsg(conn, sock, roundTripMsgs[i].type, pattern,
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
  int nodelay = 0; // as in Go: a small frame does not wait for an ACK
  socklen_t optlen = sizeof(nodelay);
  CHECK(0 == getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen));
  CHECK(0 != nodelay);
  // a child process started with exec must not inherit the sockets
  CHECK(sock < 0 || 0 != (fcntl(sock, F_GETFD) & FD_CLOEXEC));
  CHECK(sock < 0 || 0 != (fcntl(sock, F_GETFL) & O_NONBLOCK));
  CHECK(0 != (fcntl(srv.socket, F_GETFD) & FD_CLOEXEC));
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
  struct SXConnection cli = mkConn(cliTypes, NULL);
  cli.SXDataType[tOne] = &otherSizeType;
  expectHandshakeFailure(mkConn(srvTypes, NULL), cli); // SizeBytes

  cli = mkConn(cliTypes, NULL);
  cli.MaxSize++;
  expectHandshakeFailure(mkConn(srvTypes, NULL), cli); // MaxSize

  cli = mkConn(cliTypes, NULL);
  cli.SXDataType[200] = &noDataType;
  // a type without data on one side only
  expectHandshakeFailure(mkConn(srvTypes, NULL), cli);

#ifdef SX_ENCRYPTION
  expectHandshakeFailure(mkConn(srvTypes, KEY128),
                         mkConn(cliTypes, NULL)); // encryption on one side

#endif
}

// --- frames the server has to refuse

static const uint8_t *rawFrame;
static size_t rawFrameLen;

// The library makes its sockets non-blocking; a test client that calls send
// and recv by itself wants the plain blocking behaviour back.
static void blocking(int sock) {
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK);
}

static int rawClient(struct SXConnection *conn, int sock) {
  (void)conn;
  blocking(sock);
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

static void testDuplex(const char *key);

#ifdef SX_ENCRYPTION

// --- another key fails the handshake on both sides

static void testWrongKey(void) {
  struct SXConnection srv = mkConn(srvTypes, KEY128);
  struct client cl = {.conn = mkConn(cliTypes, "01112233445566778899AABBCCDDEEFF"),
                      .fn = noop};
  int sock = start(&srv, &cl);
  CHECK(-9 == sock);
  finish(&srv, &cl, sock);
  CHECK(-9 == cl.connectRc);
}

// --- a peer without the key that sends back what it gets does not pass the
// handshake (it did in protocol 1.1)

#define HELLO 1324
#define CONFIRM 16

static int tcpConnect(uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  CHECK(0 == connect(sock, (struct sockaddr *)&addr, sizeof(addr)));
  return sock;
}

struct echo {
  uint16_t port;
  uint8_t hello[HELLO]; // of the server
};

static void *echoMain(void *arg) {
  struct echo *e = arg;
  uint8_t confirm[CONFIRM];
  int sock = tcpConnect(e->port);
  if (recv(sock, e->hello, HELLO, MSG_WAITALL) == HELLO &&
      send(sock, e->hello, HELLO, 0) == HELLO &&
      recv(sock, confirm, CONFIRM, MSG_WAITALL) == CONFIRM)
    send(sock, confirm, CONFIRM, 0);
  close(sock);
  return NULL;
}

static void testHelloEcho(void) {
  struct SXConnection srv = mkConn(srvTypes, KEY128);
  struct echo e[2];
  CHECK(0 == SXListen("127.0.0.1", 0, &srv));
  for (int i = 0; i < 2; i++) {
    pthread_t thread;
    e[i].port = listenPort(&srv);
    CHECK(0 == pthread_create(&thread, NULL, echoMain, &e[i]));
    CHECK(-9 == SXAccpet(&srv));
    pthread_join(thread, NULL);
  }
  SXClose(srv.socket);
  // a new nonce for every connection: a recorded session cannot be replayed
  CHECK(0 != memcmp(e[0].hello + 8, e[1].hello + 8, 32));
}

// --- somebody in the path: a relay between the client and the server that
// changes the first record of the client, or sends it a second time

enum { relayChange, relayRepeat };

struct relay {
  int listener, mode;
  uint16_t serverPort;
};

static void *relayMain(void *arg) {
  struct relay *r = arg;
  static uint8_t sent[4096]; // what the client sent after the handshake
  size_t total = 0;
  int client = accept(r->listener, NULL, NULL);
  int server = tcpConnect(r->serverPort);
  struct pollfd fds[2] = {{.fd = client, .events = POLLIN},
                          {.fd = server, .events = POLLIN}};
  while (poll(fds, 2, 5000) > 0) {
    uint8_t buf[4096];
    if (fds[1].revents) {
      ssize_t n = recv(server, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      send(client, buf, (size_t)n, 0);
    }
    if (fds[0].revents) {
      ssize_t n = recv(client, buf, sizeof(buf), 0);
      if (n <= 0) { // the client is done
        if (relayRepeat == r->mode) // length, data and type, tag
          send(server, sent, 4 + (sent[0] | (size_t)sent[1] << 8) + CONFIRM,
               0);
        break;
      }
      for (ssize_t i = 0; i < n; i++, total++) {
        size_t pos = total - (HELLO + CONFIRM);
        if (total >= HELLO + CONFIRM && pos < sizeof(sent))
          sent[pos] = buf[i];
        if (relayChange == r->mode && 4 == pos) // the first byte of the data
          buf[i] ^= 1;
      }
      send(server, buf, (size_t)n, 0);
    }
  }
  if (relayRepeat == r->mode)
    usleep(300000); // the server reads the repeated record before the EOF
  close(client);
  close(server);
  return NULL;
}

static int oneMsgClient(struct SXConnection *conn, int sock) {
  return SXWriteMsg(conn, sock, tOne, "hello", 5);
}

static void testRelay(int mode) {
  struct SXConnection srv = mkConn(srvTypes, KEY128);
  struct client cl = {.conn = mkConn(cliTypes, KEY128), .fn = oneMsgClient};
  memset(&srvSeen, 0, sizeof(srvSeen));
  CHECK(0 == SXListen("127.0.0.1", 0, &srv));

  struct relay r = {.mode = mode, .serverPort = listenPort(&srv)};
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t len = sizeof(addr);
  r.listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(0 == bind(r.listener, (struct sockaddr *)&addr, sizeof(addr)));
  CHECK(0 == listen(r.listener, 1));
  CHECK(0 == getsockname(r.listener, (struct sockaddr *)&addr, &len));
  cl.port = ntohs(addr.sin_port);

  pthread_t relayThread;
  CHECK(0 == pthread_create(&relayThread, NULL, relayMain, &r));
  CHECK(0 == pthread_create(&cl.thread, NULL, clientMain, &cl));
  int sock = SXAccpet(&srv);
  CHECK(sock >= 0);
  if (sock >= 0) {
    if (relayRepeat == mode) // the record itself is good, once
      CHECK(0 == SXProcessMsg(&srv, sock));
    CHECK(-3 == SXProcessMsg(&srv, sock));
    CHECK(srvSeen.count == (relayRepeat == mode ? 1 : 0));
  }
  finish(&srv, &cl, sock);
  pthread_join(relayThread, NULL);
  close(r.listener);
  CHECK(0 == cl.connectRc);
  CHECK(0 == cl.rc);
}

// --- an invalid key is refused before any I/O

static void testBadKey(void) {
  const char *keys[] = {"zz", KEY128 "00"}; // not hex, 17 bytes
  for (int i = 0; i < 2; i++) {
    struct SXConnection conn = mkConn(srvTypes, keys[i]);
    conn.socket = -1;
    // not -6: a handshake failure can be retried, a bad key cannot
    CHECK(-8 == SXListen("127.0.0.1", 0, &conn));
    CHECK(-8 == SXConnect("127.0.0.1", 1, &conn));
    CHECK(-1 == conn.socket); // nothing was opened
  }
}

// --- a key without cipher state on the socket: an error, not a frame in the
// clear

static void testNoPlaintextFallback(void) {
  struct SXConnection conn = mkConn(srvTypes, KEY128);
  int sv[2];
  CHECK(0 == socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  CHECK(-5 == SXWriteMsg(&conn, sv[0], tOne, "hi", 2));
  close(sv[0]);
  close(sv[1]);
}

// --- the format follows the socket: a connection made with a key stays
// encrypted when AESKey is cleared afterwards

static int clearedKeyClient(struct SXConnection *conn, int sock) {
  conn->AESKey = NULL;
  int rc = SXWriteMsg(conn, sock, tOne, "hello", 5);
  return 0 != rc ? rc : SXProcessMsg(conn, sock); // the reply of the server
}

static void testKeyClearedAfterConnect(void) {
  struct SXConnection srv = mkConn(srvTypes, KEY128);
  struct client cl = {.conn = mkConn(cliTypes, KEY128),
                      .fn = clearedKeyClient};
  memset(&srvSeen, 0, sizeof(srvSeen));
  memset(&cliSeen, 0, sizeof(cliSeen));

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0) {
    CHECK(0 == SXProcessMsg(&srv, sock)); // the server still has the key
    CHECK(0 == SXWriteMsg(&srv, sock, tOne, "reply", 5));
  }
  finish(&srv, &cl, sock);

  CHECK(0 == cl.rc);
  CHECK(1 == srvSeen.count && 0 == memcmp(srvSeen.data, "hello", 5));
  CHECK(1 == cliSeen.count && 0 == memcmp(cliSeen.data, "reply", 5));
}

static void testEncryption(void) {
  testRoundTrip(KEY128);
  testRoundTrip(KEY192);
  testRoundTrip(KEY256);
  testDuplex(KEY128);
  testWrongKey();
  testHelloEcho();
  testRelay(relayChange);
  testRelay(relayRepeat);
  testBadKey();
  testNoPlaintextFallback();
  testKeyClearedAfterConnect();
}

#else

// --- a library built without SX_ENCRYPTION refuses a key before any I/O

static void testEncryption(void) {
  struct SXConnection conn = mkConn(srvTypes, KEY128);
  conn.socket = -1;
  CHECK(-7 == SXListen("127.0.0.1", 0, &conn));
  CHECK(-7 == SXConnect("127.0.0.1", 1, &conn));
  CHECK(-7 == SXAccpet(&conn));
  CHECK(-1 == conn.socket); // nothing was opened
}

#endif

// --- SXWriteMsg refuses what the peer could not read; nothing is sent

static void testWriteLimits(void) {
  struct SXConnection conn = mkConn(srvTypes, NULL);
  conn.SXDataType[tBad] = &badType;
  const void *buf = pattern;

  CHECK(-1 == SXWriteMsg(&conn, -1, 200, buf, 1));           // undefined type
  CHECK(-2 == SXWriteMsg(&conn, -1, tFixed, buf, FIXED - 1)); // not FixedSize
  CHECK(-2 == SXWriteMsg(&conn, -1, tFixed, buf, FIXED + 1));
  CHECK(-2 == SXWriteMsg(&conn, -1, tEmpty, buf, 1)); // data for a type without
  CHECK(-3 == SXWriteMsg(&conn, -1, tOne, buf, 256));
  CHECK(-3 == SXWriteMsg(&conn, -1, tTwo, buf, 0x10000));
  CHECK(-3 == SXWriteMsg(&conn, -1, tThree, buf, MAXSIZE + 1));
  CHECK(-4 == SXWriteMsg(&conn, -1, tBad, buf, 1));

  conn.MaxSize = 0; // only messages without data, as in the Go version
  CHECK(-3 == SXWriteMsg(&conn, -1, tOne, buf, 1));
}

// --- writing to a connection closed by the peer is an error, not SIGPIPE

static void testWriteToClosedPeer(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = noop};

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

// --- ReadTimeout covers a whole read; signals neither fail nor extend it

// {tOne, 10 bytes} sent one byte per 50 ms: 0.8 s for the frame
static int slowClient(struct SXConnection *conn, int sock) {
  (void)conn;
  blocking(sock);
  uint8_t frame[16] = {tOne, 10};
  uint32_t crc = sxCrc32(pattern, 10);
  memcpy(frame + 2, pattern, 10);
  memcpy(frame + 12, &crc, sizeof(crc));
  for (size_t i = 0; i < sizeof(frame); i++) {
    if (send(sock, frame + i, 1, 0) != 1)
      return -1;
    usleep(50000);
  }
  return 0;
}

static void testSlowPeer(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  srv.ReadTimeout = (struct timeval){.tv_usec = 150000};
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = slowClient};

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0) // every byte comes in time, the 14 bytes of body and crc don't
    CHECK(-2 == SXProcessMsg(&srv, sock));
  finish(&srv, &cl, sock);
}

// a 70000-byte message in two halves with a signal for the reader in between
static int signalClient(struct SXConnection *conn, int sock) {
  (void)conn;
  blocking(sock);
  uint32_t size = sizeof(pattern), crc = sxCrc32(pattern, size);
  uint8_t header[4] = {tThree, size & 0xff, (size >> 8) & 0xff, size >> 16};
  size_t half = size / 2;
  if (send(sock, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
      send(sock, pattern, half, 0) != (ssize_t)half)
    return -1;
  usleep(50000); // the server waits for the second half
  pthread_kill(mainThread, SIGUSR1);
  usleep(50000);
  if (send(sock, pattern + half, size - half, 0) != (ssize_t)(size - half) ||
      send(sock, &crc, sizeof(crc), 0) != (ssize_t)sizeof(crc))
    return -1;
  return 0;
}

static void testSignalDuringRead(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = signalClient};
  memset(&srvSeen, 0, sizeof(srvSeen));

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0)
    CHECK(0 == SXProcessMsg(&srv, sock));
  finish(&srv, &cl, sock);
  CHECK(0 == cl.rc);
  CHECK(sizeof(pattern) == srvSeen.size);
  CHECK(0 == memcmp(srvSeen.data, pattern, sizeof(pattern)));
}

// sends nothing, signals the reader every 50 ms for 0.6 s
static int pesterClient(struct SXConnection *conn, int sock) {
  (void)conn, (void)sock;
  for (int i = 0; i < 12; i++) {
    pthread_kill(mainThread, SIGUSR1);
    usleep(50000);
  }
  return 0;
}

static void testSignalsKeepReadTimeout(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  srv.ReadTimeout = (struct timeval){.tv_usec = 200000};
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = pesterClient};

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0) {
    double t0 = seconds();
    CHECK(-2 == SXProcessMsg(&srv, sock));
    CHECK(seconds() - t0 < 0.45); // the timeout, not the client leaving
  }
  finish(&srv, &cl, sock);
}

// --- WriteTimeout covers a whole frame

// takes up to 16 KB every 50 ms for 1.5 s: the writer always makes a little
// progress. The socket stays non-blocking: the writer gives up long before
// the end and nothing more comes.
static int drainClient(struct SXConnection *conn, int sock) {
  (void)conn;
  static uint8_t sink[16384];
  int small = sizeof(sink);
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  for (int i = 0; i < 30; i++) {
    if (0 == recv(sock, sink, sizeof(sink), 0))
      break;
    usleep(50000);
  }
  return 0;
}

static void testWriteTimeout(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  struct client cl = {.conn = mkConn(cliTypes, NULL), .fn = drainClient};
  srv.MaxSize = cl.conn.MaxSize = sizeof(big);
  srv.WriteTimeout = (struct timeval){.tv_usec = 300000};

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0) {
    int small = 16384;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    double t0 = seconds();
    CHECK(-5 == SXWriteMsg(&srv, sock, tThree, big, sizeof(big)));
    CHECK(seconds() - t0 < 1.2); // while the client still reads
  }
  finish(&srv, &cl, sock);
}

// --- ConnectTimeout ends SXConnect; signals neither fail nor extend it

// signals the main thread every 50 ms, 6 times
static void *pester(void *arg) {
  (void)arg;
  for (int i = 0; i < 6; i++) {
    usleep(50000);
    pthread_kill(mainThread, SIGUSR1);
  }
  return NULL;
}

// SXConnect to TEST-NET-1 (RFC 5737) with a ConnectTimeout of 0.2 s, with or
// without signals for the main thread meanwhile. Returns the time it took.
static double connectToNowhere(int signals) {
  struct SXConnection conn = mkConn(cliTypes, NULL);
  conn.ConnectTimeout = (struct timeval){.tv_usec = 200000};
  pthread_t thread;
  if (signals)
    CHECK(0 == pthread_create(&thread, NULL, pester, NULL));
  double t0 = seconds();
  CHECK(-4 == SXConnect("192.0.2.1", 9, &conn));
  double took = seconds() - t0;
  if (signals)
    pthread_join(thread, NULL);
  return took;
}

static void testConnectTimeout(void) {
  double took = connectToNowhere(0);
  CHECK(took < 1.5);
  if (took < 0.15) { // no route, or something in the path refuses the address
    puts("note: 192.0.2.1 was rejected at once, ConnectTimeout not tested");
    return;
  }

  // Three attempts: on the network of the first run about 1 connect of 20 was
  // refused within 10 ms. A signal that fails the connect ends all of them
  // after 50 ms.
  int timedOut = 0;
  for (int i = 0; i < 3 && !timedOut; i++) {
    took = connectToNowhere(1);
    timedOut = took > 0.15 && took < 0.45;
  }
  CHECK(timedOut);
}

// --- a signal ends a waiting SXAccpet: the way to stop an accept loop

static void testAcceptSignal(void) {
  struct SXConnection srv = mkConn(srvTypes, NULL);
  CHECK(0 == SXListen("127.0.0.1", 0, &srv));
  pthread_t thread;
  CHECK(0 == pthread_create(&thread, NULL, pester, NULL));
  errno = 0;
  CHECK(-1 == SXAccpet(&srv));
  CHECK(EINTR == errno);
  pthread_join(thread, NULL);
  SXClose(srv.socket);
}

// --- one thread reads while another one writes on the same socket

#define DUPLEX_MSGS 200

struct writer {
  struct SXConnection *conn;
  int sock, rc;
};

static void *writerMain(void *arg) {
  struct writer *w = arg;
  for (int i = 0; 0 == w->rc && i < DUPLEX_MSGS; i++)
    w->rc = SXWriteMsg(w->conn, w->sock, tTwo, pattern, 1000);
  return NULL;
}

// Writes DUPLEX_MSGS messages from a second thread and reads as many here.
static int duplex(struct SXConnection *conn, int sock) {
  struct writer w = {conn, sock, 0};
  pthread_t thread;
  if (0 != pthread_create(&thread, NULL, writerMain, &w))
    return -100;
  int rc = 0;
  for (int i = 0; 0 == rc && i < DUPLEX_MSGS; i++)
    rc = SXProcessMsg(conn, sock);
  pthread_join(thread, NULL);
  return 0 != rc ? rc : w.rc;
}

static void testDuplex(const char *key) {
  struct SXConnection srv = mkConn(srvTypes, key);
  struct client cl = {.conn = mkConn(cliTypes, key), .fn = duplex};
  memset(&srvSeen, 0, sizeof(srvSeen));
  memset(&cliSeen, 0, sizeof(cliSeen));

  int sock = start(&srv, &cl);
  CHECK(sock >= 0);
  if (sock >= 0)
    CHECK(0 == duplex(&srv, sock));
  finish(&srv, &cl, sock);
  CHECK(0 == cl.connectRc);
  CHECK(0 == cl.rc);
  CHECK(DUPLEX_MSGS == srvSeen.count && DUPLEX_MSGS == cliSeen.count);
  CHECK(1000 == srvSeen.size && 0 == memcmp(srvSeen.data, pattern, 1000));
  CHECK(1000 == cliSeen.size && 0 == memcmp(cliSeen.data, pattern, 1000));
}

// --- listen and connect failures

static void testListenAndConnectErrors(void) {
  struct SXConnection first = mkConn(srvTypes, NULL);
  struct SXConnection second = mkConn(srvTypes, NULL);
  CHECK(0 == SXListen("127.0.0.1", 0, &first));
  uint16_t port = listenPort(&first);

  second.socket = 12345;
  CHECK(-3 == SXListen("127.0.0.1", port, &second)); // the port is taken
  CHECK(-1 == second.socket); // not a stale descriptor for SXClose
  SXClose(first.socket);

  second.socket = 12345;
  CHECK(-4 == SXConnect("127.0.0.1", port, &second)); // nobody listens
  CHECK(-1 == second.socket); // not a stale descriptor for SXClose

  CHECK(0 == SXListen(NULL, 0, &first)); // every interface
  SXClose(first.socket);
}

int main(void) {
  // a parent that ignores SIGPIPE would hide a missing protection
  signal(SIGPIPE, SIG_DFL);

  // no SA_RESTART: a blocked call sees every signal
  mainThread = pthread_self();
  struct sigaction sa = {.sa_handler = onSignal};
  sigaction(SIGUSR1, &sa, NULL);

  for (size_t i = 0; i < sizeof(pattern); i++)
    pattern[i] = (uint8_t)(i * 7);

  CHECK(0 == SXInit());
  testRoundTrip(NULL);
  testEncryption();
  testHandshakeMismatch();
  testRefusedFrames();
  testWriteLimits();
  testWriteToClosedPeer();
  testReadTimeout();
  testSlowPeer();
  testSignalDuringRead();
  testSignalsKeepReadTimeout();
  testWriteTimeout();
  testConnectTimeout();
  testAcceptSignal();
  testDuplex(NULL);
  testListenAndConnectErrors();

  if (failures > 0) {
    printf("%d checks FAILED\n", failures);
    return 1;
  }
#ifdef SX_ENCRYPTION
  puts("C tests OK (with encryption)");
#else
  puts("C tests OK (without encryption)");
#endif
  return 0;
}
