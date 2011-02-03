/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_ftruncate --
 *	Truncate a file.
 */
int
__wt_ftruncate(WT_TOC *toc, WT_FH *fh, off_t len)
{
	ENV *env;

	env = toc->env;

	if (ftruncate(fh->fd, len) == 0) {
		fh->file_size = len;
		return (0);
	}

	__wt_api_env_err(env, errno, "%s ftruncate error", fh->name);
	return (WT_ERROR);
}
