/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_handle_sync --
 *	Flush a file handle.
 */
static int
__wt_handle_sync(int fd)
{
	WT_DECL_RET;

#if defined(F_FULLFSYNC)
	/*
	 * OS X fsync documentation:
	 * "Note that while fsync() will flush all data from the host to the
	 * drive (i.e. the "permanent storage device"), the drive itself may
	 * not physically write the data to the platters for quite some time
	 * and it may be written in an out-of-order sequence. For applications
	 * that require tighter guarantees about the integrity of their data,
	 * Mac OS X provides the F_FULLFSYNC fcntl. The F_FULLFSYNC fcntl asks
	 * the drive to flush all buffered data to permanent storage."
	 *
	 * OS X F_FULLFSYNC fcntl documentation:
	 * "This is currently implemented on HFS, MS-DOS (FAT), and Universal
	 * Disk Format (UDF) file systems."
	 */
	WT_SYSCALL_RETRY(fcntl(fd, F_FULLFSYNC, 0), ret);
	if (ret == 0)
		return (0);
	/*
	 * Assume F_FULLFSYNC failed because the file system doesn't support it
	 * and fallback to fsync.
	 */
#endif
#if defined(HAVE_FDATASYNC)
	WT_SYSCALL_RETRY(fdatasync(fd), ret);
#else
	WT_SYSCALL_RETRY(fsync(fd), ret);
#endif
	return (ret);
}

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

	if ((ret = __wt_handle_sync(fh->fd)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: fsync", fh->name);
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
__wt_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
#ifdef __linux__
	WT_DECL_RET;
	int fd, tret;
	const char *dir;
	char *copy;

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

	WT_SYSCALL_RETRY(((fd =
	    open(path, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
	__wt_free(session, copy);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: open", path);

	if ((ret = __wt_handle_sync(fd)) != 0)
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

	if ((ret = __wt_handle_sync(fh->fd)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s fsync error", fh->name);
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

	WT_SYSCALL_RETRY(sync_file_range(fh->fd,
	    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: sync_file_range", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	return (0);
#endif
}
