CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

all: bserve bcurl

bserve: src/bserve.c
	$(CC) $(CFLAGS) -o $@ $<

bcurl: src/bcurl.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	./tests/run.sh

clean:
	rm -f bserve bcurl

.PHONY: all test clean
