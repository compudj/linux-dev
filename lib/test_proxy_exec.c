// SPDX-License-Identifier: GPL-2.0
/*
 * Test module for proxy execution: kernel mutexes that user space can
 * hold, take and chain.
 *
 * Proxy execution acts on tasks blocked on a struct mutex. User space
 * cannot hold a kernel mutex for a chosen time, nor make two tasks take
 * two of them in a chosen order. This module lets it do both. See
 * tools/testing/selftests/sched/proxy_exec.
 *
 * Write one command to <debugfs>/test_proxy_exec/op:
 *   hold I MS        lock mutex I, busy-wait MS ms, unlock
 *   hold_sleep I MS  lock mutex I, sleep MS ms, unlock
 *   take I           lock mutex I, unlock
 *   take_intr I      mutex_lock_interruptible(I), unlock if acquired
 *   pair A B MS      lock A, busy-wait MS ms, lock B killable, unlock both
 * The write returns when the command is done. It fails with EINTR if a
 * signal interrupted take_intr or the inner lock of pair.
 *
 * Two tasks that run pair with the mutexes in opposite orders deadlock.
 * A fatal signal ends the deadlock.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define TPE_NR_MUTEX	8
#define TPE_MAX_MS	10000

static struct mutex tpe_mutex[TPE_NR_MUTEX];
/* One lockdep class per mutex: "pair" nests them. */
static struct lock_class_key tpe_key[TPE_NR_MUTEX];
static struct dentry *tpe_dir;

/*
 * Busy-wait @ms milliseconds. The caller can be preempted. cond_resched()
 * gives a kernel without preemption a place to do it.
 */
static void tpe_busy_wait(unsigned int ms)
{
	ktime_t end = ktime_add_ms(ktime_get(), ms);

	while (ktime_before(ktime_get(), end)) {
		cpu_relax();
		cond_resched();
	}
}

static ssize_t tpe_op_write(struct file *file, const char __user *ubuf,
			    size_t len, loff_t *ppos)
{
	unsigned int a, b, ms = 0;
	char buf[64];
	int ret = 0;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "hold_sleep %u %u", &a, &ms) == 2) {
		if (a >= TPE_NR_MUTEX || ms > TPE_MAX_MS)
			return -EINVAL;
		mutex_lock(&tpe_mutex[a]);
		msleep(ms);
		mutex_unlock(&tpe_mutex[a]);
	} else if (sscanf(buf, "hold %u %u", &a, &ms) == 2) {
		if (a >= TPE_NR_MUTEX || ms > TPE_MAX_MS)
			return -EINVAL;
		mutex_lock(&tpe_mutex[a]);
		tpe_busy_wait(ms);
		mutex_unlock(&tpe_mutex[a]);
	} else if (sscanf(buf, "take_intr %u", &a) == 1) {
		if (a >= TPE_NR_MUTEX)
			return -EINVAL;
		ret = mutex_lock_interruptible(&tpe_mutex[a]);
		if (!ret)
			mutex_unlock(&tpe_mutex[a]);
	} else if (sscanf(buf, "take %u", &a) == 1) {
		if (a >= TPE_NR_MUTEX)
			return -EINVAL;
		mutex_lock(&tpe_mutex[a]);
		mutex_unlock(&tpe_mutex[a]);
	} else if (sscanf(buf, "pair %u %u %u", &a, &b, &ms) == 3) {
		if (a >= TPE_NR_MUTEX || b >= TPE_NR_MUTEX || a == b ||
		    ms > TPE_MAX_MS)
			return -EINVAL;
		mutex_lock(&tpe_mutex[a]);
		tpe_busy_wait(ms);
		ret = mutex_lock_killable(&tpe_mutex[b]);
		if (!ret)
			mutex_unlock(&tpe_mutex[b]);
		mutex_unlock(&tpe_mutex[a]);
	} else {
		return -EINVAL;
	}
	return ret ? ret : len;
}

static const struct file_operations tpe_op_fops = {
	.owner	= THIS_MODULE,
	.write	= tpe_op_write,
	.llseek	= noop_llseek,
};

static int __init tpe_init(void)
{
	int i;

	for (i = 0; i < TPE_NR_MUTEX; i++) {
		mutex_init(&tpe_mutex[i]);
		lockdep_set_class(&tpe_mutex[i], &tpe_key[i]);
	}
	tpe_dir = debugfs_create_dir("test_proxy_exec", NULL);
	debugfs_create_file("op", 0200, tpe_dir, NULL, &tpe_op_fops);
	return 0;
}
module_init(tpe_init);

static void __exit tpe_exit(void)
{
	debugfs_remove_recursive(tpe_dir);
}
module_exit(tpe_exit);

MODULE_DESCRIPTION("Kernel mutexes for the proxy execution selftests");
MODULE_LICENSE("GPL");
