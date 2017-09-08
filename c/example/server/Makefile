GIT_VERSION := $(shell git describe --long --dirty --always --tags)
CFLAGS = -Wall -std=c11 -DGIT_VERSION=\"$(GIT_VERSION)\"

DESTDIR?=/
PREFIX?=/usr

all: server-example

server-example: server-example.o
	gcc -o server-example server-example.o ../../libsxchnge.a
