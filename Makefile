CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -fwrapv -fPIC

# Set to /usr/local to install in the system directories
PREFIX = $(HOME)/prefix


# ---------------------------------------------------------

ifeq ($(PREFIX),)
	PREFIX = /usr/local
endif

SRC_FILES = $(wildcard *.c)
HEADER_FILES = $(wildcard *.h)
OBJECT_FILES = $(patsubst %.c,%.o,$(SRC_FILES))

UNAME = $(shell uname)

ifeq ($(UNAME),Darwin)
	DYLIB_EXT = dylib
	DYLIB_FLAGS = -dynamiclib
else
	DYLIB_EXT = so
	DYLIB_FLAGS = -shared -fPIC
endif


# Don't remove intermediate files
.SECONDARY:


.PHONY: all clean install uninstall remake reinstall dynamiclib staticlib test

all: dynamiclib staticlib test

clean:
	rm -f *.$(DYLIB_EXT) *.a *.o
	make -C test clean

install: all
	install libpostl.$(DYLIB_EXT) $(PREFIX)/lib
	install libpostl.a $(PREFIX)/lib
	install postl.h $(PREFIX)/include

uninstall:
	rm -f $(PREFIX)/lib/libpostl.$(DYLIB_EXT)
	rm -f $(PREFIX)/lib/libpostl.a
	rm -f $(PREFIX)/include/postl.h

remake: clean all

reinstall: clean install

dynamiclib: libpostl.$(DYLIB_EXT)

staticlib: libpostl.a

test: libpostl.a
	make -C test


%.o: %.c $(HEADER_FILES)
	$(CC) $(CFLAGS) -c -o $@ $<

%.$(DYLIB_EXT): $(OBJECT_FILES)
	$(CC) $(CFLAGS) $(DYLIB_FLAGS) -o $@ $^ -lm

%.a: $(OBJECT_FILES)
	ar -cr $@ $^
