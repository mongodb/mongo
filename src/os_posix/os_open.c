/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __open_directory --
 *	Open up a file handle to a directory.
 */
static int
__open_directory(WT_SESSION_IMPL *session, char *path, int *fd)
{
	WT_DECL_RET;

	WT_SYSCALL_RETRY(((*fd =
	    open(path, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: open_directory", path);
	return (ret);
}

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session,
    const char *name, int ok_create, int exclusive, int dio_type, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh, *tfh;
	mode_t mode;
	uint64_t bucket, hash;
	int direct_io, f, fd, matched;
	char *path;

	conn = S2C(session);
	direct_io = 0;
	fh = NULL;
	fd = -1;
	path = NULL;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: open", name));

	/* Increment the reference count if we already have the file open. */
	matched = 0;
	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_FOREACH(tfh, &conn->fhhash[bucket], hashq) {
		if (strcmp(name, tfh->name) == 0) {
			++tfh->ref;
			*fhp = tfh;
			matched = 1;
			break;
		}
	}
	__wt_spin_unlock(session, &conn->fh_lock);
	if (matched)
		return (0);

	WT_RET(__wt_filename(session, name, &path));

	if (dio_type == WT_FILE_TYPE_DIRECTORY) {
		WT_ERR(__open_directory(session, path, &fd));
		goto setupfh;
	}

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
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
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
	if (dio_type && FLD_ISSET(conn->direct_io, dio_type)) {
		f |= O_DIRECT;
		direct_io = 1;
	}
#endif
	if (dio_type == WT_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC))
#ifdef O_DSYNC
		f |= O_DSYNC;
#elif defined(O_SYNC)
		f |= O_SYNC;
#else
		WT_ERR_MSG(session, ENOTSUP,
		    "Unsupported log sync mode requested");
#endif
	WT_SYSCALL_RETRY(((fd = open(path, f, mode)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret,
		    direct_io ?
		    "%s: open failed with direct I/O configured, some "
		    "filesystem types do not support direct I/O" : "%s", path);

setupfh:
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
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
		WT_ERR(posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM));
#endif

	WT_ERR(__wt_calloc_one(session, &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));
	fh->name_hash = hash;
	fh->fd = fd;
	fh->ref = 1;
	fh->direct_io = direct_io;

	/* Set the file's size. */
	WT_ERR(__wt_filesize(session, fh, &fh->size));

	/* Configure file extension. */
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
		fh->extend_len = conn->data_extend_len;

	/* Configure fallocate/posix_fallocate calls. */
	__wt_fallocate_config(session, fh);

	/*
	 * Repeat the check for a match, but then link onto the database's list
	 * of files.
	 */
	matched = 0;
	__wt_spin_lock(session, &conn->fh_lock);
	TAILQ_FOREACH(tfh, &conn->fhhash[bucket], hashq) {
		if (strcmp(name, tfh->name) == 0) {
			++tfh->ref;
			*fhp = tfh;
			matched = 1;
			break;
		}
	}
	if (!matched) {
		WT_CONN_FILE_INSERT(conn, fh, bucket);
		(void)WT_ATOMIC_ADD4(conn->open_file_count, 1);
		*fhp = fh;
	}
	__wt_spin_unlock(session, &conn->fh_lock);
	if (matched) {
err:		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
		if (fd != -1)
			(void)close(fd);
	}

	__wt_free(session, path);
	return (ret);
}

/*
 * __wt_close --
 *	Close a file handle.
 */
int
__wt_close(WT_SESSION_IMPL *session, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	uint64_t bucket;

	conn = S2C(session);

	if (*fhp == NULL)
		return (0);
	fh = *fhp;
	*fhp = NULL;

	__wt_spin_lock(session, &conn->fh_lock);
	if (fh == NULL || fh->ref == 0 || --fh->ref > 0) {
		__wt_spin_unlock(session, &conn->fh_lock);
		return (0);
	}

	/* Remove from the list. */
	bucket = fh->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_FILE_REMOVE(conn, fh, bucket);
	(void)WT_ATOMIC_SUB4(conn->open_file_count, 1);

	__wt_spin_unlock(session, &conn->fh_lock);

	/* Discard the memory. */
	if (close(fh->fd) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "close: %s", fh->name);
	}

	__wt_free(session, fh->name);
	__wt_free(session, fh);
	return (ret);
}
