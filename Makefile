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
LDFLAGS += -Wl,-soname,$(LIBFILE)

CFLAGS += -DBINGEWATCH_LOCAL

ifeq ($(debug),on)
LCFLAGS += -ggdb
CFLAGS += -ggdb
endif

TEST_CFLAGS=\
    -ggdb \
    -DBINGEWATCH_LOCAL \

.IGNORE: clean
.PHONY: install clean uninstall

TESTLIBS = \
    -lradpool \
    -lpthread \
    -luuid \

BUF = \
	block-list-buf.c \
	flexible-ring-buf.c \
	fixed-block-buf.c \

SDR = \
    sdr-rx-machine.c \
    soapy-machine.c \

MACHINES = \
	socket-machine.c \
	file-machine.c \
	fifo-machine.c \
	null-machine.c \
    $(BUF) \
	$(SDR) \

FILTERS = \
	filters.c \
	conversions.c \

SRC = \
	machine.c \
	machine-mgmt.c \
	filter.c \
	stream.c \
	$(MACHINES) \
	$(FILTERS) \

$(LIB): $(SRC)
	$(CC) $(CFLAGS) $^ $(INC) $(LDFLAGS) $(LCFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -I/usc/local/include -Werror -ggdb -c $^

buffer-test: machine.c machine-mgmt.c filter.c $(BUF)
	$(CC) $(TEST_CFLAGS) test/simple-buffer-test.c $^ $(INC) -o test/bin/simple-buffer-test $(TESTLIBS)

file-test: machine.c machine-mgmt.c filter.c file-machine.c null-machine.c
	$(CC) $(TEST_CFLAGS) test/file-test.c $^ $(INC) -o test/bin/file-test $(TESTLIBS)

sock-test: machine.c machine-mgmt.c filter.c socket-machine.c
	$(CC) $(TEST_CFLAGS) test/sock-test.c $^ $(INC) -o test/bin/sock-test $(TESTLIBS)

tests: buffer-test file-test

install: $(LIB)
	install -m 0755 $(LIB) -D $(DESTDIR)$(libdir)/$(LIBFILE)
	cd $(DESTDIR)$(libdir); \
		ln -f -s $(LIB).so.$(VERSION) $(LIB).so.$(SO_VERSION); \
		ln -f -s $(LIB).so.$(SO_VERSION) $(LIB).so
	mkdir -p $(DESTDIR)$(includedir)/$(HDR_DIR)
	install -m 0644 -t $(DESTDIR)$(includedir)/$(HDR_DIR) include/*
ifneq ($(NOLDCONFIG),y)
	ldconfig
endif

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
