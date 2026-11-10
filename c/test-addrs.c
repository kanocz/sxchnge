// Test of SXConnect to a host with two addresses: the first one never answers,
// the second one listens. The resolver is replaced for that, so this is a
// program of its own and test.c keeps the real getaddrinfo.
#define _DEFAULT_SOURCE

#include "sxchnge.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct sockaddr_in addrs[2];
static struct addrinfo infos[2];

// "two.test" is 192.0.2.1 (TEST-NET-1, RFC 5737) and then loopback, every
// other host is loopback only
int getaddrinfo(const char *host, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
  (void)hints;
  static const char *ips[2] = {"192.0.2.1", "127.0.0.1"};
  for (int i = 0; i < 2; i++) {
    addrs[i] = (struct sockaddr_in){.sin_family = AF_INET,
                                    .sin_port = htons((uint16_t)atoi(service))};
    inet_pton(AF_INET, ips[i], &addrs[i].sin_addr);
    infos[i] = (struct addrinfo){.ai_family = AF_INET,
                                 .ai_socktype = SOCK_STREAM,
                                 .ai_addr = (struct sockaddr *)&addrs[i],
                                 .ai_addrlen = sizeof(addrs[i]),
                                 .ai_next = 0 == i ? &infos[1] : NULL};
  }
  *res = NULL != host && 0 == strcmp(host, "two.test") ? &infos[0] : &infos[1];
  return 0;
}

void freeaddrinfo(struct addrinfo *res) { (void)res; }

int main(void) {
  struct SXConnection srv = {.MaxSize = 100};
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (0 != SXListen("127.0.0.1", 0, &srv) ||
      0 != getsockname(srv.socket, (struct sockaddr *)&addr, &len)) {
    puts("FAIL: no listener on loopback");
    return 1;
  }

  // Nobody accepts: the TCP connect to the listener succeeds and the handshake
  // times out, -6. When the first address uses up ConnectTimeout the result
  // is -4. On a network that rejects 192.0.2.1 at once this passes either way.
  struct SXConnection cli = {.MaxSize = 100,
                             .ReadTimeout = {.tv_usec = 100000},
                             .WriteTimeout = {.tv_sec = 1},
                             .ConnectTimeout = {.tv_sec = 1}};
  int rc = SXConnect("two.test", ntohs(addr.sin_port), &cli);
  SXClose(srv.socket);
  if (-6 != rc) {
    printf("FAIL: SXConnect returned %d, want -6: the second address was not "
           "reached\n",
           rc);
    return 1;
  }
  puts("C address test OK");
  return 0;
}
