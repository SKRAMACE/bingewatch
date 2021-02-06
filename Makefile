# Set the processor type and import the toolchain
BUILD_CONFIG=x86_64.gcc
include tools.mk

HDR_DIR = bingewatch
LIB = libbingewatch
VERSION = 1.0.0
SO_VERSION = 0
LIBFILE=$(LIB).so.$(VERSION)

NOLDCONFIG ?= n

# Common prefix for installation directories
# NOTE: This directory must exist when you start the install
prefix ?= /usr/local
exec_prefix = $(prefix)
# Where to put library files
includedir = $(prefix)/include
# Where to put header files
libdir = $(exec_prefix)/lib

INC = -I./include
SRC_DIR = src
OUT_DIR = .
VPATH= \
    $(SRC_DIR):\
    $(SRC_DIR)/buf:\
    $(SRC_DIR)/sdr:\
    $(SRC_DIR)/filter

# Flags
LCFLAGS += -Werror -fPIC -shared
LDFLAGS += 

CFLAGS += -DBINGEWATCH_LOCAL

ifeq ($(debug),on)
LCFLAGS += -ggdb
CFLAGS += -ggdb
endif

TEST_CFLAGS=\
    -ggdb \
    -DBINGEWATCH_LOCAL \
    -Itest \

.IGNORE: clean
.PHONY: install clean uninstall all uhd soapy

TESTLIBS = \
    -lmemex \
    -lpthread \
    -luuid \

SDRLIBS = \
    -lSoapySDR \
    -luhd \

BUF = \
	block-list-buf.c \
	ring-buf.c \
	fixed-block-buf.c \

SDR = \
    sdr-rx-machine.c \

MACHINES = \
	socket-machine.c \
	file-machine.c \
	fifo-machine.c \
	null-machine.c \
    $(BUF) \

FILTERS = \
	filters.c \
	conversions.c \

SRC = \
	machine.c \
	machine-mgmt.c \
	machine-metrics.c \
	filter.c \
	stream.c \
    segment.c \
    bw-log.c \
    bw-util.c \
	$(MACHINES) \
	$(FILTERS) \
	$(SDR) \

TEST = \
	machine.c \
	machine-mgmt.c \
	machine-metrics.c \
	filter.c \
    bw-log.c \
    bw-util.c \
    test/test.c \

$(LIB): $(SRC)
	$(CC) $(CFLAGS) $^ $(INC) $(LDFLAGS) $(LCFLAGS) -o $@

libbingewatch_soapy: $(LIB) soapy-machine.c lime-machine.c
	$(CC) $(CFLAGS) $^ $(INC) $(LDFLAGS) $(LCFLAGS) -o $@

libbingewatch_uhd: $(LIB) uhd-machine.c b210-machine.c
	$(CC) $(CFLAGS) $^ $(INC) $(LDFLAGS) $(LCFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -I/usc/local/include -Werror -ggdb -c $^

ring-buffer-test: $(TEST) $(BUF)
	$(CC) $(TEST_CFLAGS) test/ring-buffer-test.c $^ $(INC) -o test/bin/$@ $(TESTLIBS)

file-test: $(TEST) file-machine.c null-machine.c
	$(CC) $(TEST_CFLAGS) test/file-test.c $^ $(INC) -o test/bin/file-test $(TESTLIBS)

stream-test: $(TEST) stream.c segment.c file-machine.c $(FILTERS) $(BUF)
	$(CC) $(TEST_CFLAGS) test/stream-test.c $^ $(INC) -o test/bin/stream-test $(TESTLIBS)

sock-test: $(TEST) socket-machine.c
	$(CC) $(TEST_CFLAGS) test/sock-test.c $^ $(INC) -o test/bin/sock-test $(TESTLIBS)

lime-test: $(TEST) $(SRC) sdr-rx-machine.c soapy-machine.c lime-machine.c
	$(CC) $(TEST_CFLAGS) test/$@.c $^ $(INC) -o test/bin/$@ $(TESTLIBS) $(SDRLIBS)

b210-test: $(TEST) $(SRC) sdr-rx-machine.c uhd-machine.c b210-machine.c
	$(CC) $(TEST_CFLAGS) test/$@.c $^ $(INC) -o test/bin/$@ $(TESTLIBS) $(SDRLIBS)

tests: buffer-test file-test stream-test

all: $(LIB) $(LIB)_soapy $(LIB)_uhd

install: $(LIB)
	@./install

uninstall:
	rm -f $(DESTDIR)$(libdir)/$(LIB)*
	rm -f $(DESTDIR)$(includedir)/$(HDR_DIR)/*

ifneq ($(NOLDCONFIG),y)
	ldconfig
endif

clean:
	rm -f *.o
	rm -f $(LIB)*
	rm -f test/bin/* 
