CFLAGS = -O2 -Wall -Wextra -std=c11

# "make ENCRYPTION=1" builds AES support in; it needs OpenSSL libcrypto.
# Objects of the other mode are not rebuilt by themselves: switch with "make -B".
ifeq ($(ENCRYPTION),1)
CFLAGS += -DSX_ENCRYPTION $(shell pkg-config --cflags libcrypto 2>/dev/null)
LDLIBS = $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)
endif

# "make -B SAN=address,undefined test" and "make -B SAN=thread test" run the
# tests under sanitizers; rebuild with "make -B" afterwards.
ifneq ($(SAN),)
CFLAGS += -g -fsanitize=$(SAN) -fno-sanitize-recover=undefined
endif

DESTDIR?=/
PREFIX?=/usr

.PHONY: all test install

all: libsxchnge.a

libsxchnge.a: sxchnge.o crc32.o
	ar rcs libsxchnge.a sxchnge.o crc32.o

test: libsxchnge.a test.c test-addrs.c
	$(CC) $(CFLAGS) -o sxtest test.c libsxchnge.a $(LDLIBS) -lpthread
	./sxtest
	$(CC) $(CFLAGS) -o sxtest-addrs test-addrs.c libsxchnge.a $(LDLIBS)
	./sxtest-addrs

install: all
	install -d -m 0755 ${DESTDIR}${PREFIX}/lib ${DESTDIR}${PREFIX}/include
	install -c -m 0644 libsxchnge.a ${DESTDIR}${PREFIX}/lib
	install -c -m 0644 sxchnge.h ${DESTDIR}${PREFIX}/include
