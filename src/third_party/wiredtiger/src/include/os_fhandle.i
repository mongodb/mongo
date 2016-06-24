/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_fsync --
 *	POSIX fsync.
 */
static inline int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_DECL_RET;
	WT_FILE_HANDLE *handle;

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-sync", fh->handle->name));

	handle = fh->handle;
	/*
	 * There is no way to check when the non-blocking sync-file-range is
	 * complete, but we track the time taken in the call for completeness.
	 */
	WT_STAT_FAST_CONN_INCR_ATOMIC(session, fsync_active);
	WT_STAT_FAST_CONN_INCR(session, fsync_io);
	if (block)
		ret = (handle->fh_sync == NULL ? 0 :
		    handle->fh_sync(handle, (WT_SESSION *)session));
	else
		ret = (handle->fh_sync_nowait == NULL ? 0 :
		    handle->fh_sync_nowait(handle, (WT_SESSION *)session));
	WT_STAT_FAST_CONN_DECR_ATOMIC(session, fsync_active);
	return (ret);
}

/*
 * __wt_fallocate --
 *	Extend a file.
 */
static inline int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;
	WT_FILE_HANDLE *handle;

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-allocate: %" PRIuMAX " at %" PRIuMAX,
	    fh->handle->name, (uintmax_t)len, (uintmax_t)offset));

	/*
	 * Our caller is responsible for handling any locking issues, all we
	 * have to do is find a function to call.
	 *
	 * Be cautious, the underlying system might have configured the nolock
	 * flavor, that failed, and we have to fallback to the locking flavor.
	 */
	handle = fh->handle;
	if (handle->fh_allocate_nolock != NULL) {
		if ((ret = handle->fh_allocate_nolock(
		    handle, (WT_SESSION *)session, offset, len)) == 0)
			return (0);
		WT_RET_ERROR_OK(ret, ENOTSUP);
	}
	if (handle->fh_allocate != NULL)
		return (handle->fh_allocate(
		    handle, (WT_SESSION *)session, offset, len));
	return (ENOTSUP);
}

/*
 * __wt_file_lock --
 *	Lock/unlock a file.
 */
static inline int
__wt_file_lock(WT_SESSION_IMPL * session, WT_FH *fh, bool lock)
{
	WT_FILE_HANDLE *handle;

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-lock: %s", fh->handle->name, lock ? "lock" : "unlock"));

	handle = fh->handle;
	return (handle->fh_lock == NULL ? 0 :
	    handle->fh_lock(handle, (WT_SESSION*)session, lock));
}

/*
 * __wt_read --
 *	POSIX pread.
 */
static inline int
__wt_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-read: %" WT_SIZET_FMT " at %" PRIuMAX,
	    fh->handle->name, len, (uintmax_t)offset));

	WT_STAT_FAST_CONN_INCR_ATOMIC(session, read_active);
	WT_STAT_FAST_CONN_INCR(session, read_io);

	ret = fh->handle->fh_read(
	    fh->handle, (WT_SESSION *)session, offset, len, buf);

	WT_STAT_FAST_CONN_DECR_ATOMIC(session, read_active);
	return (ret);
}

/*
 * __wt_filesize --
 *	Get the size of a file in bytes, by file handle.
 */
static inline int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-size", fh->handle->name));

	return (fh->handle->fh_size(fh->handle, (WT_SESSION *)session, sizep));
}

/*
 * __wt_ftruncate --
 *	POSIX ftruncate.
 */
static inline int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-truncate: %" PRIuMAX,
	    fh->handle->name, (uintmax_t)len));

	return (fh->handle->fh_truncate(
	    fh->handle, (WT_SESSION *)session, len));
}

/*
 * __wt_write --
 *	POSIX pwrite.
 */
static inline int
__wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_DECL_RET;

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY) ||
	    WT_STRING_MATCH(fh->name,
	    WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)));

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-write: %" WT_SIZET_FMT " at %" PRIuMAX,
	    fh->handle->name, len, (uintmax_t)offset));

	WT_STAT_FAST_CONN_INCR_ATOMIC(session, write_active);
	WT_STAT_FAST_CONN_INCR(session, write_io);

	ret = fh->handle->fh_write(
	    fh->handle, (WT_SESSION *)session, offset, len, buf);

	WT_STAT_FAST_CONN_DECR_ATOMIC(session, write_active);
	return (ret);
}
