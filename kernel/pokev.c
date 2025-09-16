// SPDX-License-Identifier: GPL-2.0+
/*
 * Code patch pokev system call
 */

#include <linux/syscalls.h>
#include <linux/cleanup.h>
#include <linux/pokev.h>

static
int validate_pokev(struct pokevec *pokevec, int pokevcnt)
{
	int i;

	for (i = 0; i < pokevcnt; pokevec++, i++)
		if (pokevec->len > POKE_MAX_LEN)
			return -EINVAL;
	return 0;
}

static
int do_pokev(struct pokevec *pokevec, int pokevcnt)
{
	int i;

	for (i = 0; i < pokevcnt; pokevec++, i++)
		if (access_process_vm(current, (unsigned long)pokevec->ptr,
				      pokevec->insn, pokevec->len,
				      FOLL_WRITE | FOLL_FORCE) != pokevec->len)
			return -EFAULT;
	return 0;
}

SYSCALL_DEFINE2(pokev, const struct pokevec *, pokevec, int, pokevcnt)
{
	struct pokevec *kpokevec __free(kvfree) = NULL;
	int ret;

	if (pokevcnt <= 0)
		return -EINVAL;
	kpokevec = kvzalloc(sizeof(struct pokevec) * pokevcnt, GFP_KERNEL);
	if (!kpokevec)
		return -ENOMEM;
	if (copy_from_user(kpokevec, pokevec, sizeof(struct pokevec) * pokevcnt))
		return -EFAULT;
	/* Validate input */
	ret = validate_pokev(kpokevec, pokevcnt);
	if (ret)
		return ret;
	ret = do_pokev(kpokevec, pokevcnt);
	if (ret)
		return ret;
	return 0;
}
