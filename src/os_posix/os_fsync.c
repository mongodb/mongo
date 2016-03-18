/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_directory_sync_fh --
 *	Flush a directory file handle to ensure file creation is durable.
 *
 * We don't use fsync because most file systems don't require this step and
 * we don't want to penalize them by calling fsync.
 */
int
__wt_directory_sync_fh(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

#ifdef __linux__
	return (WT_JUMP(j_handle_sync, session, fh, true));
#else
	WT_UNUSED(fh);
	return (0);
#endif
}

/*
 * __wt_directory_sync --
 *	Flush a directory to ensure file creation is durable.
 */
int
__wt_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
#ifdef __linux__
	WT_DECL_RET;
	const char *dir;
	char *copy;
#endif

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

#ifdef __linux__
	/*
	 * POSIX 1003.1 does not require that fsync of a file handle ensures the
	 * entry in the directory containing the file has also reached disk (and
	 * there are historic Linux filesystems requiring this), do an explicit
	 * fsync on a file descriptor for the directory to be sure.
	 */
	copy = NULL;
	if (path == NULL || (dir = strrchr(path, '/')) == NULL)
		path = S2C(session)->home;
	else {
		/*
		 * Copy the directory name, leaving the trailing slash in place,
		 * so a path of "/foo" doesn't result in an empty string.
		 */
		WT_RET(__wt_strndup(
		    session, path, (size_t)(dir - path) + 1, &copy));
		path = copy;
	}

	ret = WT_JUMP(j_directory_sync, session, path);

	__wt_free(session, copy);

	return (ret);
#else
	WT_UNUSED(path);
	return (0);
#endif
}
