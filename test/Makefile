CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -fwrapv

TESTS = $(patsubst %.c,%,$(wildcard *.c))

.PHONY: all clean remake

all: $(TESTS)

clean:
	rm -f $(TESTS)

remake: clean all


runpostl: runpostl.c ../libpostl.a
	$(CC) $(CFLAGS) -o $@ $^
