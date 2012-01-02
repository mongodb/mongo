/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_rename --
 *	Rename a file.
 */
int
__wt_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	int ret;
	const char *from_path, *to_path;

	WT_VERBOSE(session, fileops, "rename %s to %s", from, to);

	WT_RET(__wt_filename(session, from, &from_path));
	WT_RET(__wt_filename(session, to, &to_path));

	WT_SYSCALL_RETRY(rename(from_path, to_path), ret);

	__wt_free(session, from_path);
	__wt_free(session, to_path);

	if (ret == 0)
		return (0);

	WT_RET_MSG(session, ret, "rename %s to %s", from, to);
}
