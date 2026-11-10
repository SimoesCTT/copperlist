CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS += -Iinclude -Ivendor/chipmem/include -Ivendor/blit/include
LDLIBS += -ldrm -lm -lOpenCL

LIB_SRC = src/copper.c \
          vendor/chipmem/src/chipmem.c \
          vendor/blit/src/blit.c \
          vendor/blit/src/blit_cpu.c \
          vendor/blit/src/blit_i915.c \
          vendor/blit/src/blit_cl.c \
          vendor/blit/src/addr_alloc.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: libcopper.a

libcopper.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/t_copper: tests/t_copper.c libcopper.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -L. -lcopper $(LDLIBS) -o $@

tests/probe_sequence: tests/probe_sequence.c libcopper.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -L. -lcopper $(LDLIBS) -o $@

check: tests/t_copper
	./tests/t_copper

run-probe: tests/probe_sequence
	./tests/probe_sequence

clean:
	rm -f $(LIB_OBJ) libcopper.a tests/t_copper tests/probe_sequence

.PHONY: all check run-probe clean
