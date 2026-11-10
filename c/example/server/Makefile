GIT_VERSION := $(shell git describe --long --dirty --always --tags)
CFLAGS = -Wall -std=c11 -DGIT_VERSION=\"$(GIT_VERSION)\"

# pass ENCRYPTION=1 when the library was built with it: it then needs libcrypto
ifeq ($(ENCRYPTION),1)
LDLIBS = $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)
endif

DESTDIR?=/
PREFIX?=/usr

all: server-example

server-example: server-example.o ../../libsxchnge.a
	gcc -o server-example server-example.o ../../libsxchnge.a $(LDLIBS)
