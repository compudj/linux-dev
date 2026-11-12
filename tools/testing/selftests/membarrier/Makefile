# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -g $(KHDR_INCLUDES) -pthread -I../../../../tools/include
LDLIBS += -lpthread

TEST_GEN_PROGS := membarrier_test_single_thread \
		membarrier_test_multi_thread \
		membarrier_test_ipi \
		membarrier_test_cmd \
		membarrier_rseq_stress

LOCAL_HDRS += membarrier_test_impl.h membarrier_test_barrier.h

# membarrier_test_barrier loads BPF programs. Build it only when clang
# is there, so that the other tests do not need it.
CLANG ?= clang
ifneq ($(shell command -v $(CLANG) 2>/dev/null),)
TEST_GEN_PROGS += membarrier_test_barrier
TEST_GEN_FILES += membarrier_test_barrier.bpf.o
else
$(warning membarrier selftests: skipping membarrier_test_barrier, needs clang)
endif

include ../lib.mk
include ../net/bpf.mk

$(OUTPUT)/membarrier_test_barrier.bpf.o: membarrier_test_barrier.h
$(OUTPUT)/membarrier_test_barrier: $(BPFOBJ)
$(OUTPUT)/membarrier_test_barrier: CFLAGS += -I$(SCRATCH_DIR)/include
$(OUTPUT)/membarrier_test_barrier: LDLIBS += -lelf -lz
