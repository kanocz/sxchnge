GIT_VERSION := $(shell git describe --long --dirty --always --tags)
CFLAGS = -Wall -std=c11 -DGIT_VERSION=\"$(GIT_VERSION)\"

DESTDIR?=/
PREFIX?=/usr

all: client-example

client-example: client-example.o
	gcc -o client-example client-example.o ../../libsxchnge.a
