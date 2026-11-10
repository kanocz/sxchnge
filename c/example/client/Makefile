CFLAGS = -O2 -Wall -Wextra -std=c11

# pass ENCRYPTION=1 when the library was built with it: it then needs libcrypto
ifeq ($(ENCRYPTION),1)
LDLIBS = $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)
endif

.PHONY: all

all: client-example

client-example: client-example.o ../../libsxchnge.a
	gcc -o client-example client-example.o ../../libsxchnge.a $(LDLIBS)
