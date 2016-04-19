/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_directory_sync_fh --
 *	Flush a directory file handle to ensure file creation is durable.
 *
 * We don't use the normal sync path because many file systems don't require
 * this step and we don't want to penalize them.
 */
static inline int
__wt_directory_sync_fh(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (fh->fh_sync(session, fh, true));
}

/*
 * __wt_fallocate --
 *	Extend a file.
 */
static inline int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-allocate: %" PRIuMAX " at %" PRIuMAX,
	    fh->name, (uintmax_t)len, (uintmax_t)offset));

	return (fh->fh_allocate(session, fh, offset, len));
}

/*
 * __wt_file_lock --
 *	Lock/unlock a file.
 */
static inline int
__wt_file_lock(WT_SESSION_IMPL * session, WT_FH *fh, bool lock)
{
	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-lock: %s", fh->name, lock ? "lock" : "unlock"));

	return (fh->fh_lock(session, fh, lock));
}

/*
 * __wt_read --
 *	POSIX pread.
 */
static inline int
__wt_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-read: %" WT_SIZET_FMT " at %" PRIuMAX,
	    fh->name, len, (uintmax_t)offset));

	WT_STAT_FAST_CONN_INCR(session, read_io);

	return (fh->fh_read(session, fh, offset, len, buf));
}

/*
 * __wt_filesize --
 *	Get the size of a file in bytes, by file handle.
 */
static inline int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-size", fh->name));

	return (fh->fh_size(session, fh, sizep));
}

/*
 * __wt_fsync --
 *	POSIX fsync.
 */
static inline int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-sync", fh->name));

	return (fh->fh_sync(session, fh, block));
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
	    "%s: handle-truncate: %" PRIuMAX, fh->name, (uintmax_t)len));

	return (fh->fh_truncate(session, fh, len));
}

/*
 * __wt_write --
 *	POSIX pwrite.
 */
static inline int
__wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY) ||
	    WT_STRING_MATCH(fh->name,
	    WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)));

	WT_RET(__wt_verbose(session, WT_VERB_HANDLEOPS,
	    "%s: handle-write: %" WT_SIZET_FMT " at %" PRIuMAX,
	    fh->name, len, (uintmax_t)offset));

	WT_STAT_FAST_CONN_INCR(session, write_io);

	return (fh->fh_write(session, fh, offset, len, buf));
}
