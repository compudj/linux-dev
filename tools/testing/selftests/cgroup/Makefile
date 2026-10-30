# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -pthread $(KHDR_INCLUDES)

all: ${HELPER_PROGS}

TEST_FILES     := with_stress.sh
TEST_PROGS     := test_stress.sh test_cpuset_prs.sh test_cpuset_v1_hp.sh
TEST_GEN_FILES := wait_inotify
# Keep the lists lexicographically sorted
TEST_GEN_PROGS  = test_core
TEST_GEN_PROGS += test_cpu
TEST_GEN_PROGS += test_cpu_concurrency
TEST_GEN_PROGS += test_cpu_concurrency_stress
TEST_GEN_PROGS += test_cpuset
TEST_GEN_PROGS += test_freezer
TEST_GEN_PROGS += test_hugetlb_memcg
TEST_GEN_PROGS += test_kill
TEST_GEN_PROGS += test_kmem
TEST_GEN_PROGS += test_memcontrol
TEST_GEN_PROGS += test_pids
TEST_GEN_PROGS += test_zswap

LOCAL_HDRS += $(selfdir)/clone3/clone3_selftests.h $(selfdir)/pidfd/pidfd.h
LOCAL_HDRS += cpu_concurrency_util.h cpu_concurrency_rseq.h

include ../lib.mk
include lib/libcgroup.mk

# The rseq selftest library, built with the include paths of its own Makefile.
CC_RSEQ_O := $(OUTPUT)/cpu_concurrency_rseq.o
$(CC_RSEQ_O): cpu_concurrency_rseq.c cpu_concurrency_rseq.h \
	      $(wildcard $(selfdir)/rseq/rseq*.[ch])
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -I$(selfdir)/rseq \
		-I$(top_srcdir)/tools/include -c $< -o $@
EXTRA_CLEAN += $(CC_RSEQ_O)

$(OUTPUT)/test_core: $(LIBCGROUP_O)
$(OUTPUT)/test_cpu: $(LIBCGROUP_O)
$(OUTPUT)/test_cpu_concurrency: $(LIBCGROUP_O) cpu_concurrency_util.c $(CC_RSEQ_O)
$(OUTPUT)/test_cpu_concurrency: LDLIBS += -ldl
$(OUTPUT)/test_cpu_concurrency_stress: $(LIBCGROUP_O) cpu_concurrency_util.c
$(OUTPUT)/test_cpuset: $(LIBCGROUP_O)
$(OUTPUT)/test_freezer: $(LIBCGROUP_O)
$(OUTPUT)/test_hugetlb_memcg: $(LIBCGROUP_O)
$(OUTPUT)/test_kill: $(LIBCGROUP_O)
$(OUTPUT)/test_kmem: $(LIBCGROUP_O)
$(OUTPUT)/test_memcontrol: $(LIBCGROUP_O)
$(OUTPUT)/test_pids: $(LIBCGROUP_O)
$(OUTPUT)/test_zswap: $(LIBCGROUP_O)
