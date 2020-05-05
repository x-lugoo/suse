/*
 *  securelevel.c - support for generic kernel lockdown
 *
 *  Copyright Nebula, Inc <mjg59@srcf.ucam.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/uaccess.h>

static int securelevel;

static DEFINE_SPINLOCK(securelevel_lock);

#define MAX_SECURELEVEL 1

int get_securelevel(void)
{
	return securelevel;
}
EXPORT_SYMBOL(get_securelevel);

int set_securelevel(int new_securelevel)
{
	int ret = 0;

	spin_lock(&securelevel_lock);

	if ((securelevel == -1) || (new_securelevel < securelevel) ||
	    (new_securelevel > MAX_SECURELEVEL)) {
		ret = -EINVAL;
		goto out;
	}

	securelevel = new_securelevel;
out:
	spin_unlock(&securelevel_lock);
	return ret;
}
EXPORT_SYMBOL(set_securelevel);
