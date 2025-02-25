# SPDX-License-Identifier: GPL-2.0+ OR MIT

top_srcdir = ../../../..

CFLAGS += -O2 -Wall -g -I./ $(KHDR_INCLUDES) -L$(OUTPUT) -Wl,-rpath=./ \
	  $(CLANG_FLAGS) -I$(top_srcdir)/tools/include
LDLIBS += -lpthread

TEST_GEN_PROGS = basic_test

include ../lib.mk

$(OUTPUT)/%: %.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
