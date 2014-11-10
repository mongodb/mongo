/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_directory_sync_fh --
 *	Flush a directory file handle.  We don't use __wt_fsync because
 *	most file systems don't require this step and we don't want to
 *	penalize them by calling fsync.
 */
int
__wt_directory_sync_fh(WT_SESSION_IMPL *session, WT_FH *fh)
{
#ifdef __linux__
	WT_DECL_RET;

	WT_SYSCALL_RETRY(fsync(fh->fd), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: fsync", fh->name);
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	return (0);
#endif
}

/*
 * __wt_directory_sync --
 *	Flush a directory to ensure a file creation is durable.
 */
int
__wt_directory_sync(WT_SESSION_IMPL *session, char *path)
{
#ifdef __linux__
	WT_DECL_RET;
	int fd, tret;
	char *dir;

	/*
	 * POSIX 1003.1 does not require that fsync of a file handle ensures the
	 * entry in the directory containing the file has also reached disk (and
	 * there are historic Linux filesystems requiring this), do an explicit
	 * fsync on a file descriptor for the directory to be sure.
	 */
	if ((dir = strrchr(path, '/')) == NULL)
		path = (char *)".";
	else
		*dir = '\0';
	WT_SYSCALL_RETRY(((fd =
	    open(path, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
	if (dir != NULL)
		*dir = '/';
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: open", path);

	WT_SYSCALL_RETRY(fsync(fd), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret, "%s: fsync", path);

err:	WT_SYSCALL_RETRY(close(fd), tret);
	if (tret != 0)
		__wt_err(session, tret, "%s: close", path);
	WT_TRET(tret);
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(path);
	return (0);
#endif
}

/*
 * __wt_fsync --
 *	Flush a file handle.
 */
int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fsync", fh->name));

#ifdef HAVE_FDATASYNC
	WT_SYSCALL_RETRY(fdatasync(fh->fd), ret);
#else
	WT_SYSCALL_RETRY(fsync(fh->fd), ret);
#endif
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s fsync error", fh->name);

	return (0);
}

/*
 * __wt_fsync_async --
 *	Flush a file handle and don't wait for the result.
 */
int
__wt_fsync_async(WT_SESSION_IMPL *session, WT_FH *fh)
{
#ifdef	HAVE_SYNC_FILE_RANGE
	WT_DECL_RET;

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: sync_file_range", fh->name));

	if ((ret = sync_file_range(fh->fd,
	    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: sync_file_range", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	return (0);
#endif
}
