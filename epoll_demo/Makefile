CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
           -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS =

TARGETS = server client

.PHONY: all clean

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)
