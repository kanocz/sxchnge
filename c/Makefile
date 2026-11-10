GIT_VERSION := $(shell git describe --long --dirty --always --tags)
CFLAGS = -Wall -std=c11 -DGIT_VERSION=\"$(GIT_VERSION)\"

# "make ENCRYPTION=1" builds AES support in; it needs OpenSSL libcrypto.
# Objects of the other mode are not rebuilt by themselves: switch with "make -B".
ifeq ($(ENCRYPTION),1)
CFLAGS += -DSX_ENCRYPTION $(shell pkg-config --cflags libcrypto 2>/dev/null)
LDLIBS = $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)
endif

DESTDIR?=/
PREFIX?=/usr

all: libsxchnge.a

libsxchnge.a: sxchnge.o crc32.o
	ar rcs libsxchnge.a sxchnge.o crc32.o

test: libsxchnge.a test.c
	$(CC) $(CFLAGS) -o sxtest test.c libsxchnge.a $(LDLIBS) -lpthread
	./sxtest

install: all
	install -d -m 0755 ${DESTDIR}${PREFIX}/lib ${DESTDIR}${PREFIX}/include
	install -c -m 0644 libsxchnge.a ${DESTDIR}${PREFIX}/lib
	install -c -m 0644 sxchnge.h ${DESTDIR}${PREFIX}/include
