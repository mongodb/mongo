/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_rename --
 *	Rename a file.
 */
int
__wt_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_DECL_RET;
	uint32_t lasterror;
	char *from_path, *to_path;

	WT_RET(__wt_verbose(
		session, WT_VERB_FILEOPS, "rename %s to %s", from, to));

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));
	from_path = to_path = NULL;

	WT_RET(__wt_filename(session, from, &from_path));
	WT_TRET(__wt_filename(session, to, &to_path));

	/*
	 * Check if file exists since Windows does not override the file if
	 * it exists.
	 */
	if ((ret = GetFileAttributesA(to_path)) != INVALID_FILE_ATTRIBUTES) {
		if ((ret = DeleteFileA(to_path)) == FALSE) {
			lasterror = __wt_errno();
			goto err;
		}
	}

	if ((MoveFileA(from_path, to_path)) == FALSE)
		lasterror = __wt_errno();

err:
	__wt_free(session, from_path);
	__wt_free(session, to_path);

	if (ret != FALSE)
		return (0);

	WT_RET_MSG(session, lasterror, "MoveFile %s to %s", from, to);
}
