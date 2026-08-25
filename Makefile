# cvisor - Linux x86-64 only (see README.md §2)
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -g -O2
LDLIBS   = -lncurses -ldw

SRCS := src/main.c src/analyzer.c src/recorder.c src/trace.c src/tui.c \
        src/dwarfvars.c
OBJS := $(SRCS:.c=.o)

# test targets: OSTEP-compatible build flags are mandatory (spec §4)
TESTCFLAGS := -g -O0 -no-pie -fno-omit-frame-pointer
TESTSRCS   := $(wildcard tests/*.c)
TESTBINS   := $(TESTSRCS:.c=)

# libc-free assembly targets: entry is _start, syscalls are written by hand.
# `as -g` is what emits the DWARF line table cvisor requires (spec §6.1) —
# without it analyze() bails out.  ld's default output is already ET_EXEC,
# so there is no -no-pie equivalent to pass here.
AS         ?= as
LD         ?= ld
ASFLAGS    ?= -g
TESTASMSRCS := $(wildcard tests/*.s)
TESTASMBINS := $(TESTASMSRCS:.s=)

# ch4 example programs: cvisor-observation builds only
CH4SRCS := $(wildcard ch4/*.c)
CH4BINS := $(CH4SRCS:.c=)
CH4ASMSRCS := $(wildcard ch4/*.s)
CH4ASMBINS := $(CH4ASMSRCS:.s=)

# ch6 measurement homework: -O2 for real numbers, *_cv for viewing in cvisor
CH6SRCS := $(wildcard ch6/*.c)
CH6BINS := $(CH6SRCS:.c=)
CH6CV   := $(CH6SRCS:.c=_cv)

# ch6 assembly (swtch.s): already -O0 by construction, so there is no
# separate _cv build — the one binary is both runnable and traceable
CH6ASMSRCS := $(wildcard ch6/*.s)
CH6ASMBINS := $(CH6ASMSRCS:.s=)

all: cvisor

cvisor: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/cvisor.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests: $(TESTBINS) $(TESTASMBINS)

tests/%: tests/%.c
	$(CC) $(TESTCFLAGS) -o $@ $<

# the source file must end up next to the binary: analyzer.c open_source()
# looks for the CU path, then basename(CU) beside the target, then in cwd
tests/%: tests/%.s
	$(AS) $(ASFLAGS) -o $@.o $<
	$(LD) -o $@ $@.o
	rm -f $@.o

ch4: $(CH4BINS) $(CH4ASMBINS)

ch4/%: ch4/%.c
	$(CC) $(TESTCFLAGS) -Wall -Wextra -o $@ $<

ch4/%: ch4/%.s
	$(AS) $(ASFLAGS) -o $@.o $<
	$(LD) -o $@ $@.o
	rm -f $@.o

ch6: $(CH6BINS) $(CH6CV) $(CH6ASMBINS)

ch6/%_cv: ch6/%.c
	$(CC) $(TESTCFLAGS) -Wall -Wextra -o $@ $<

ch6/%: ch6/%.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

ch6/%: ch6/%.s
	$(AS) $(ASFLAGS) -o $@.o $<
	$(LD) -o $@ $@.o
	rm -f $@.o

check: cvisor tests
	./cvisor --dump tests/showcase > /dev/null && echo "dump: OK"
	./cvisor --trace tests/showcase | tail -3
	./cvisor --trace --from-main tests/showcase | tail -3

clean:
	rm -f cvisor $(OBJS) $(TESTBINS) $(TESTASMBINS) \
	      $(CH4BINS) $(CH4ASMBINS) $(CH6BINS) $(CH6CV) $(CH6ASMBINS)
	rm -f $(addsuffix .o,$(TESTASMBINS) $(CH4ASMBINS) $(CH6ASMBINS))

.PHONY: all tests ch4 ch6 check clean
