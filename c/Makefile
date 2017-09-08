GIT_VERSION := $(shell git describe --long --dirty --always --tags)
CFLAGS = -Wall -std=c11 -DGIT_VERSION=\"$(GIT_VERSION)\"

DESTDIR?=/
PREFIX?=/usr

all: libsxchnge.a

libsxchnge.a: sxchnge.o
	ar rcs libsxchnge.a sxchnge.o

install: all
	install -d -m 0755 ${DESTDIR}${PREFIX}/lib
	install -c -m 0644 libsxchnge.a ${DESTDIR}${PREFIX}/lib
	install -c -m 0644 sxchnge.h ${DESTDIR}${PREFIX}/include
