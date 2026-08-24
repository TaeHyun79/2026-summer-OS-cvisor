# cvisor - Linux x86-64 only (see README.md §2)
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -g -O2
LDLIBS   = -lncurses

SRCS := src/main.c src/analyzer.c src/recorder.c src/trace.c src/tui.c
OBJS := $(SRCS:.c=.o)

# test targets: OSTEP-compatible build flags are mandatory (spec §4)
TESTCFLAGS := -g -O0 -no-pie -fno-omit-frame-pointer
TESTSRCS   := $(wildcard tests/*.c)
TESTBINS   := $(TESTSRCS:.c=)

all: cvisor

cvisor: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/cvisor.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests: $(TESTBINS)

tests/%: tests/%.c
	$(CC) $(TESTCFLAGS) -o $@ $<

check: cvisor tests
	./cvisor --dump tests/factorial > /dev/null && echo "dump: OK"
	./cvisor --trace tests/factorial | tail -3
	./cvisor --trace --from-main tests/globals | tail -3
	./cvisor --trace --from-main tests/crash | tail -3

clean:
	rm -f cvisor $(OBJS) $(TESTBINS)

.PHONY: all tests check clean
