/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cond_wait --
 *	Wait on a mutex, optionally timing out.
 */
static inline int
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs)
{
	bool notused;

	return (__wt_cond_wait_signal(session, cond, usecs, &notused));
}

/*
 * __wt_strdup --
 *	ANSI strdup function.
 */
static inline int
__wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
{
	return (__wt_strndup(
	    session, str, (str == NULL) ? 0 : strlen(str), retp));
}

/*
 * __wt_seconds --
 *	Return the seconds since the Epoch.
 */
static inline int
__wt_seconds(WT_SESSION_IMPL *session, time_t *timep)
{
	struct timespec t;

	WT_RET(__wt_epoch(session, &t));

	*timep = t.tv_sec;

	return (0);
}

/*
 * __wt_verbose --
 * 	Verbose message.
 */
static inline int
__wt_verbose(WT_SESSION_IMPL *session, int flag, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
#ifdef HAVE_VERBOSE
	WT_DECL_RET;
	va_list ap;

	if (WT_VERBOSE_ISSET(session, flag)) {
		va_start(ap, fmt);
		ret = __wt_eventv(session, true, 0, NULL, 0, fmt, ap);
		va_end(ap);
	}
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(flag);
	WT_UNUSED(fmt);
	return (0);
#endif
}

/*
 * __wt_dirlist --
 *	Get a list of files from a directory.
 */
static inline int
__wt_dirlist(WT_SESSION_IMPL *session, const char *dir,
    const char *prefix, uint32_t flags, char ***dirlist, u_int *countp)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: directory-list: %s prefix %s",
	    dir, LF_ISSET(WT_DIRLIST_INCLUDE) ? "include" : "exclude",
	    prefix == NULL ? "all" : prefix));

	return (S2C(session)->file_directory_list(
	    session, dir, prefix, flags, dirlist, countp));
}

/*
 * __wt_directory_sync --
 *	Flush a directory to ensure file creation is durable.
 */
static inline int
__wt_directory_sync(WT_SESSION_IMPL *session, const char *name)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: directory-sync", name));

	return (S2C(session)->file_directory_sync(session, name));
}

/*
 * __wt_exist --
 *	Return if the file exists.
 */
static inline int
__wt_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: file-exist", name));

	return (S2C(session)->file_exist(session, name, existp));
}

/*
 * __wt_remove --
 *	POSIX remove.
 */
static inline int
__wt_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: file-remove", name));

	return (S2C(session)->file_remove(session, name));
}

/*
 * __wt_rename --
 *	POSIX rename.
 */
static inline int
__wt_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s to %s: file-rename", from, to));

	return (S2C(session)->file_rename(session, from, to));
}

/*
 * __wt_filesize_name --
 *	Get the size of a file in bytes, by file name.
 */
static inline int
__wt_filesize_name(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: file-size", name));

	return (S2C(session)->file_size(session, name, silent, sizep));
}

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
 * __wt_vfprintf --
 *	ANSI C vfprintf.
 */
static inline int
__wt_vfprintf(WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	WT_RET(__wt_verbose(
	    session, WT_VERB_HANDLEOPS, "%s: handle-printf", fh->name));

	return (fh->fh_printf(session, fh, fmt, ap));
}

/*
 * __wt_fprintf --
 *	ANSI C fprintf.
 */
static inline int
__wt_fprintf(WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vfprintf(session, fh, fmt, ap);
	va_end(ap);

	return (ret);
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
 *	POSIX fflush/fsync.
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
	    "%s: handle-truncate: %" PRIuMAX,
	    fh->name, (uintmax_t)len));

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
