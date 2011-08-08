/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_exist --
 *	Return if the file exists.
 */
int
__wt_exist(const char *path)
{
	struct stat sb;
	int ret;

	/*
	 * XXX
	 * This isn't correct: EINTR doesn't mean the file doesn't exist.
	 */
	SYSCALL_RETRY(stat(path, &sb), ret);
	return (ret == 0 ? 1 : 0);
}
