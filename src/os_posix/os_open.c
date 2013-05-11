/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __open_directory_sync:
 *	Fsync the directory in which we created the file.
 */
static int
__open_directory_sync(WT_SESSION_IMPL *session)
{
#ifdef __linux__
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int fd;

	conn = S2C(session);

	/*
	 * According to the Linux fsync man page:
	 *	Calling fsync() does not necessarily ensure that the entry in
	 *	the directory containing the file has also reached disk. For
	 *	that an explicit fsync() on a file descriptor for the directory
	 *	is also needed.
	 *
	 * Open the WiredTiger home directory and sync it, I don't want the rest
	 * of the system to have to wonder if opening a file creates it.
	 */
	WT_SYSCALL_RETRY(((fd =
	    open(conn->home, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: open", conn->home);
	WT_SYSCALL_RETRY(fsync(fd), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret, "%s: fsync", conn->home);
err:	WT_SYSCALL_RETRY(close(fd), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret, "%s: close", conn->home);
#else
	WT_UNUSED(session);
#endif
	return (0);
}

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session,
    const char *name, int ok_create, int exclusive, int is_tree, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	mode_t mode;
	int direct_io, f, fd, matched;
	const char *path;

	conn = S2C(session);
	fh = NULL;
	fd = -1;
	direct_io = 0;

	WT_VERBOSE_RET(session, fileops, "%s: open", name);

	/* Increment the reference count if we already have the file open. */
	matched = 0;
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (strcmp(name, fh->name) == 0) {
			++fh->refcnt;
			*fhp = fh;
			matched = 1;
			break;
		}
	}
	__wt_spin_unlock(session, &conn->fh_lock);
	if (matched)
		return (0);

	WT_RET(__wt_filename(session, name, &path));

	f = O_RDWR;
#ifdef O_BINARY
	/* Windows clones: we always want to treat the file as a binary. */
	f |= O_BINARY;
#endif
#ifdef O_CLOEXEC
	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles.
	 */
	f |= O_CLOEXEC;
#endif
#ifdef O_NOATIME
	/* Avoid updating metadata for read-only workloads. */
	if (is_tree)
		f |= O_NOATIME;
#endif

	if (ok_create) {
		f |= O_CREAT;
		if (exclusive)
			f |= O_EXCL;
		mode = 0666;
	} else
		mode = 0;

#ifdef O_DIRECT
	if (is_tree && FLD_ISSET(conn->direct_io, WT_DIRECTIO_DATA)) {
		f |= O_DIRECT;
		direct_io = 1;
	}
#endif

	WT_SYSCALL_RETRY(((fd = open(path, f, mode)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret,
		    direct_io ?
		    "%s: open failed with direct I/O configured, some "
		    "filesystem types do not support direct I/O" : "%s", name);

#if defined(HAVE_FCNTL) && defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles.  There's an obvious
	 * race here, so we prefer the flag to open if available.
	 */
	if ((f = fcntl(fd, F_GETFD)) == -1 ||
	    fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1)
		WT_ERR_MSG(session, __wt_errno(), "%s: fcntl", name);
#endif

#if defined(HAVE_POSIX_FADVISE)
	/* Disable read-ahead on trees: it slows down random read workloads. */
	if (is_tree)
		WT_ERR(posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM));
#endif

	if (F_ISSET(S2C(session), WT_CONN_SYNC))
		WT_ERR(__open_directory_sync(session));

	WT_ERR(__wt_calloc(session, 1, sizeof(WT_FH), &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));
	fh->fd = fd;
	fh->refcnt = 1;

#ifdef O_DIRECT
	if (f & O_DIRECT)
		fh->direct_io = 1;
#endif

	/* Set the file's size. */
	WT_ERR(__wt_filesize(session, fh, &fh->size));

	/* Link onto the environment's list of files. */
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_INSERT_TAIL(&conn->fhqh, fh, q);
	WT_CSTAT_INCR(session, file_open);
	__wt_spin_unlock(session, &conn->fh_lock);

	*fhp = fh;

	if (0) {
err:		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
		if (fd != -1)
			(void)close(fd);
	}

	__wt_free(session, path);

	WT_UNUSED(is_tree);			/* Only used in #ifdef's. */
	return (ret);
}

/*
 * __wt_close --
 *	Close a file handle.
 */
int
__wt_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	if (fh == NULL || fh->refcnt == 0 || --fh->refcnt > 0)
		return (0);

	/* Remove from the list and discard the memory. */
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_REMOVE(&conn->fhqh, fh, q);
	WT_CSTAT_DECR(session, file_open);
	__wt_spin_unlock(session, &conn->fh_lock);

	if (close(fh->fd) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "%s", fh->name);
	}

	__wt_free(session, fh->name);
	__wt_free(session, fh);
	return (ret);
}
