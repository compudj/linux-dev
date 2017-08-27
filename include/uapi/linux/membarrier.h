#ifndef _UAPI_LINUX_MEMBARRIER_H
#define _UAPI_LINUX_MEMBARRIER_H

/*
 * linux/membarrier.h
 *
 * membarrier system call API
 *
 * Copyright (c) 2010, 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * enum membarrier_cmd - membarrier system call command
 * @MEMBARRIER_CMD_QUERY:   Query the set of supported commands. It returns
 *                          a bitmask of valid commands.
 * @MEMBARRIER_CMD_SHARED:  Execute a memory barrier on all running threads.
 *                          Upon return from system call, the caller thread
 *                          is ensured that all running threads have passed
 *                          through a state where all memory accesses to
 *                          user-space addresses match program order between
 *                          entry to and return from the system call
 *                          (non-running threads are de facto in such a
 *                          state). This covers threads from all processes
 *                          running on the system. This command returns 0.
 * @MEMBARRIER_CMD_PRIVATE_EXPEDITED:
 *                          Execute a memory barrier on each running
 *                          thread belonging to the same process as the current
 *                          thread. Upon return from system call, the
 *                          caller thread is ensured that all its running
 *                          threads siblings have passed through a state
 *                          where all memory accesses to user-space
 *                          addresses match program order between entry
 *                          to and return from the system call
 *                          (non-running threads are de facto in such a
 *                          state). This only covers threads from the
 *                          same processes as the caller thread. This
 *                          command returns 0. The "expedited" commands
 *                          complete faster than the non-expedited ones,
 *                          they usually never block, but have the
 *                          downside of causing extra overhead. The only
 *                          case where it can block is the first time it
 *                          is called by a process with the
 *                          MEMBARRIER_FLAG_SYNC_CORE flag, if there has
 *                          not been any prior registration of that
 *                          process with
 *                          MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED
 *                          and the same flag.
 * @MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
 *                          When used with MEMBARRIER_FLAG_SYNC_CORE,
 *                          register the current process as requiring
 *                          core serialization when a private expedited
 *                          membarrier is issued. It may block. It can
 *                          be used to ensure
 *                          MEMBARRIER_CMD_PRIVATE_EXPEDITED never
 *                          blocks, even the first time it is invoked by
 *                          a process with the MEMBARRIER_FLAG_SYNC_CORE
 *                          flag.
 *
 * Command to be passed to the membarrier system call. The commands need to
 * be a single bit each, except for MEMBARRIER_CMD_QUERY which is assigned to
 * the value 0.
 */
enum membarrier_cmd {
	MEMBARRIER_CMD_QUERY				= 0,
	MEMBARRIER_CMD_SHARED				= (1 << 0),
	/* reserved for MEMBARRIER_CMD_SHARED_EXPEDITED (1 << 1) */
	/* reserved for MEMBARRIER_CMD_PRIVATE (1 << 2) */
	MEMBARRIER_CMD_PRIVATE_EXPEDITED		= (1 << 3),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED	= (1 << 4),
};

enum membarrier_flags {
	MEMBARRIER_FLAG_SYNC_CORE			= (1 << 0),
};

#endif /* _UAPI_LINUX_MEMBARRIER_H */
