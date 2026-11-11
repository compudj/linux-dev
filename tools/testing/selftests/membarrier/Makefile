# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -g $(KHDR_INCLUDES) -pthread -I../../../../tools/include
LDLIBS += -lpthread

TEST_GEN_PROGS := membarrier_test_single_thread \
		membarrier_test_multi_thread \
		membarrier_rseq_stress

LOCAL_HDRS += membarrier_test_impl.h

include ../lib.mk
