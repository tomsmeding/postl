CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -fwrapv

TESTS = $(patsubst %.c,%,$(wildcard *.c))

.PHONY: all clean remake

all: $(TESTS)

clean:
	rm -f $(TESTS)

remake: clean all


runpostl: runpostl.c ../libpostl.a
	$(CC) $(CFLAGS) -o $@ $^ -lm

repl: repl.c ../libpostl.a
	$(CC) $(CFLAGS) -I/usr/local/opt/readline/include -L/usr/local/opt/readline/lib -o $@ $^ -lreadline -lm
