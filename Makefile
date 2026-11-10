CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS += -Iinclude -Ivendor/chipmem/include -Ivendor/blit/include
LDLIBS += -ldrm -lm -lOpenCL

VENDOR_SRC = $(wildcard vendor/chipmem/src/*.c) \
             $(wildcard vendor/blit/src/*.c) \
             src/copper.c

PROBE = tests/probe_sequence
TEST  = tests/t_copper

$(PROBE): tests/probe_sequence.c $(VENDOR_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/probe_sequence.c $(VENDOR_SRC) $(LDLIBS) -o $@
$(TEST): tests/t_copper.c $(VENDOR_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/t_copper.c $(VENDOR_SRC) $(LDLIBS) -o $@

run-probe: $(PROBE)
	./$(PROBE)
check: $(TEST)
	./$(TEST)
clean:
	rm -f $(PROBE) $(TEST)
.PHONY: run-probe check clean
