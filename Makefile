# cvisor - Linux x86-64 only (see README.md §2)
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -g -O2
LDLIBS   = -lncurses -ldw

SRCS := src/main.c src/analyzer.c src/recorder.c src/trace.c src/tui.c \
        src/dwarfvars.c
OBJS := $(SRCS:.c=.o)

# observation targets: OSTEP-compatible build flags are mandatory (spec §4)
TESTCFLAGS := -g -O0 -no-pie -fno-omit-frame-pointer

# libc-free assembly targets: entry is _start, syscalls are written by hand.
# `as -g` is what emits the DWARF line table cvisor requires (spec §6.1) —
# without it analyze() bails out.  ld's default output is already ET_EXEC,
# so there is no -no-pie equivalent to pass here.
AS         ?= as
LD         ?= ld
ASFLAGS    ?= -g

# ch5 example programs: cvisor-observation builds only
CH5SRCS := $(wildcard ch5/*.c)
CH5BINS := $(CH5SRCS:.c=)
CH5ASMSRCS := $(wildcard ch5/*.s)
CH5ASMBINS := $(CH5ASMSRCS:.s=)

# ch6 measurement homework: -O2 for real numbers, *_cv for viewing in cvisor
CH6SRCS := $(wildcard ch6/*.c)
CH6BINS := $(CH6SRCS:.c=)
CH6CV   := $(CH6SRCS:.c=_cv)

# ch6 assembly (swtch.s): already -O0 by construction, so there is no
# separate _cv build — the one binary is both runnable and traceable
CH6ASMSRCS := $(wildcard ch6/*.s)
CH6ASMBINS := $(CH6ASMSRCS:.s=)

# ch13 address-space examples: cvisor-observation builds only (like ch5)
CH13SRCS := $(wildcard ch13/*.c)
CH13BINS := $(CH13SRCS:.c=)

all: cvisor

cvisor: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/cvisor.h
	$(CC) $(CFLAGS) -c -o $@ $<

# NOTE: the source file must end up next to the binary — analyzer.c
# open_source() looks for the CU path, then basename(CU) beside the
# target, then in cwd.
ch5: $(CH5BINS) $(CH5ASMBINS)

ch5/%: ch5/%.c
	$(CC) $(TESTCFLAGS) -Wall -Wextra -o $@ $<

ch5/%: ch5/%.s
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

ch13: $(CH13BINS)

ch13/%: ch13/%.c
	$(CC) $(TESTCFLAGS) -Wall -Wextra -o $@ $<

check: cvisor ch5
	./cvisor --dump ch5/p1 > /dev/null && echo "dump: OK"
	./cvisor --trace --from-main ch5/p1 | tail -3

clean:
	rm -f cvisor $(OBJS) \
	      $(CH5BINS) $(CH5ASMBINS) $(CH6BINS) $(CH6CV) $(CH6ASMBINS) \
	      $(CH13BINS)
	rm -f $(addsuffix .o,$(CH5ASMBINS) $(CH6ASMBINS))

.PHONY: all ch5 ch6 ch13 check clean
