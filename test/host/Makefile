# Host test harness for src/z3660.c -- compiles the REAL driver on host gcc
# against a mock piscsi mailbox and byte-checks its CD-ROM/disk CDB synthesis.
#
#   make -C test/host check     # build + run (exit 0 == gating assertions pass)
#   make -C test/host clean
#
# The driver is K&R C: -std=gnu89 -fno-builtin.  z3660.c is compiled in a
# "kernel-only" include world (-Istubs shadows sys/types.h, sys/immu.h,
# sys/errno.h, rico.h, sd.h) with the WRLONG/RDLONG seam force-included
# (mock_regs.h) and HOST_TEST defined.  The harness .c files are ordinary host
# code and do NOT get -Istubs (so they see the real system headers).

CC       := gcc
CSTD     := -std=gnu89 -fno-builtin
CWARN    := -Wall -Wno-unused-parameter -Wno-unused-variable
CFLAGS   := $(CSTD) $(CWARN) -O0 -g
BUILD    := build
DRV      := ../../src/z3660.c

HOST_OBJS := $(BUILD)/mock_piscsi.o $(BUILD)/kstubs.o $(BUILD)/z3660_test.o
OBJS      := $(BUILD)/z3660.o $(HOST_OBJS)
BIN       := $(BUILD)/z3660_test

.PHONY: all check clean
all: $(BIN)

# The driver: kernel include world + register seam + HOST_TEST.
$(BUILD)/z3660.o: $(DRV) mock_regs.h stubs/rico.h stubs/sd.h \
                  stubs/sys/types.h stubs/sys/immu.h stubs/sys/errno.h \
                  stubs/sys/inline.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHOST_TEST -Istubs -include mock_regs.h -c $(DRV) -o $@

# Harness translation units: plain host code (real system headers).
$(BUILD)/%.o: %.c mock_piscsi.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

check: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
