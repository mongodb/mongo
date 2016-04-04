/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __posix_sync --
 *	Underlying support function to flush a file handle.
 */
static int
__posix_sync(WT_SESSION_IMPL *session,
    int fd, const char *name, const char *func, bool block)
{
	WT_DECL_RET;

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

#ifdef	HAVE_SYNC_FILE_RANGE
	if (!block) {
		WT_SYSCALL_RETRY(sync_file_range(fd,
		    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE), ret);
		if (ret == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: %s: sync_file_range", name, func);
	}
#else
	/*
	 * Callers attempting asynchronous flush handle ENOTSUP returns, and
	 * won't make further attempts.
	 */
	if (!block)
		return (ENOTSUP);
#endif

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
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: %s: fdatasync", name, func);
#else
	WT_SYSCALL_RETRY(fsync(fd), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: %s: fsync", name, func);
#endif
}

/*
 * __posix_directory_sync --
 *	Flush a directory to ensure file creation is durable.
 */
static int
__posix_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
#ifdef __linux__
	WT_DECL_RET;
	int fd, tret;
	const char *dir;
	char *copy;

	tret = 0;
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

	WT_SYSCALL_RETRY((
	    (fd = open(path, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret, "%s: directory-sync: open", path);

	ret = __posix_sync(session, fd, path, "directory-sync", true);

	WT_SYSCALL_RETRY(close(fd), tret);
	if (tret != 0) {
		__wt_err(session, tret, "%s: directory-sync: close", path);
		if (ret == 0)
			ret = tret;
	}
err:	__wt_free(session, copy);
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(path);
	return (0);
#endif
}

/*
 * __posix_file_exist --
 *	Return if the file exists.
 */
static int
__posix_file_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	struct stat sb;
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, name, &path));
	name = path;

	WT_SYSCALL_RETRY(stat(name, &sb), ret);
	if (ret == 0)
		*existp = true;
	else if (ret == ENOENT) {
		*existp = false;
		ret = 0;
	} else
		__wt_err(session, ret, "%s: file-exist: stat", name);

	__wt_free(session, path);
	return (ret);
}

/*
 * __posix_file_remove --
 *	Remove a file.
 */
static int
__posix_file_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;
	char *path;

#ifdef HAVE_DIAGNOSTIC
	if (__wt_handle_search(session, name, false, NULL, NULL))
		WT_RET_MSG(session, EINVAL,
		    "%s: file-remove: file has open handles", name);
#endif

	WT_RET(__wt_filename(session, name, &path));
	name = path;

	WT_SYSCALL_RETRY(remove(name), ret);
	if (ret != 0)
		__wt_err(session, ret, "%s: file-remove: remove", name);

	__wt_free(session, path);
	return (ret);
}

/*
 * __posix_file_rename --
 *	Rename a file.
 */
static int
__posix_file_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_DECL_RET;
	char *from_path, *to_path;

#ifdef HAVE_DIAGNOSTIC
	if (__wt_handle_search(session, from, false, NULL, NULL))
		WT_RET_MSG(session, EINVAL,
		    "%s: file-rename: file has open handles", from);
	if (__wt_handle_search(session, to, false, NULL, NULL))
		WT_RET_MSG(session, EINVAL,
		    "%s: file-rename: file has open handles", to);
#endif

	from_path = to_path = NULL;
	WT_ERR(__wt_filename(session, from, &from_path));
	from = from_path;
	WT_ERR(__wt_filename(session, to, &to_path));
	to = to_path;

	WT_SYSCALL_RETRY(rename(from, to), ret);
	if (ret != 0)
		__wt_err(session, ret,
		    "%s to %s: file-rename: rename", from, to);

err:	__wt_free(session, from_path);
	__wt_free(session, to_path);
	return (ret);
}

/*
 * __posix_file_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
__posix_file_size(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	struct stat sb;
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, name, &path));
	name = path;

	/*
	 * Optionally don't log errors on ENOENT; some callers of this function
	 * expect failure in that case and don't want an error message logged.
	 */
	WT_SYSCALL_RETRY(stat(name, &sb), ret);
	if (ret == 0)
		*sizep = sb.st_size;
	else if (ret != ENOENT || !silent)
		__wt_err(session, ret, "%s: file-size: stat", name);

	__wt_free(session, path);

	return (ret);
}

/*
 * __posix_handle_advise --
 *	POSIX fadvise.
 */
static int
__posix_handle_advise(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, wt_off_t len, int advice)
{
#if defined(HAVE_POSIX_FADVISE)
	WT_DECL_RET;

	/*
	 * Refuse pre-load when direct I/O is configured for the file, the
	 * kernel cache isn't interesting.
	 */
	if (advice == POSIX_MADV_WILLNEED && fh->direct_io)
		return (ENOTSUP);

	WT_SYSCALL_RETRY(posix_fadvise(fh->fd, offset, len, advice), ret);
	if (ret == 0)
		return (0);

	/*
	 * Treat EINVAL as not-supported, some systems don't support some flags.
	 * Quietly fail, callers expect not-supported failures.
	 */
	if (ret == EINVAL)
		return (ENOTSUP);

	WT_RET_MSG(session, ret, "%s: handle-advise: posix_fadvise", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(advice);

	/* Quietly fail, callers expect not-supported failures. */
	return (ENOTSUP);
#endif
}

/*
 * __posix_handle_close --
 *	ANSI C close/fclose.
 */
static int
__posix_handle_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_DECL_RET;

	if (fh->fp == NULL) {
		WT_SYSCALL_RETRY(close(fh->fd), ret);
		if (ret == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: handle-close: close", fh->name);
	}

	/* If the stream was opened for writing, flush the file. */
	if (F_ISSET(fh, WT_FH_FLUSH_ON_CLOSE) && fflush(fh->fp) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "%s: handle-close: fflush", fh->name);
	}

	/* Close the file. */
	if (fclose(fh->fp) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "%s: handle-close: fclose", fh->name);
	}
	return (ret);
}

/*
 * __posix_handle_getc --
 *	ANSI C fgetc.
 */
static int
__posix_handle_getc(WT_SESSION_IMPL *session, WT_FH *fh, int *chp)
{
	if (fh->fp == NULL)
		WT_RET_MSG(session,
		    ENOTSUP, "%s: handle-getc: no stream configured", fh->name);

	*chp = fgetc(fh->fp);
	if (*chp != EOF || !ferror(fh->fp))
		return (0);
	WT_RET_MSG(session, __wt_errno(), "%s: handle-getc: fgetc", fh->name);
}

/*
 * __posix_handle_lock --
 *	Lock/unlock a file.
 */
static int
__posix_handle_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	struct flock fl;
	WT_DECL_RET;

	/*
	 * WiredTiger requires this function be able to acquire locks past
	 * the end of file.
	 *
	 * Note we're using fcntl(2) locking: all fcntl locks associated with a
	 * file for a given process are removed when any file descriptor for the
	 * file is closed by the process, even if a lock was never requested for
	 * that file descriptor.
	 */
	fl.l_start = 0;
	fl.l_len = 1;
	fl.l_type = lock ? F_WRLCK : F_UNLCK;
	fl.l_whence = SEEK_SET;

	WT_SYSCALL_RETRY(fcntl(fh->fd, F_SETLK, &fl), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: handle-lock: fcntl", fh->name);
}

/*
 * __posix_handle_printf --
 *	ANSI C vfprintf.
 */
static int
__posix_handle_printf(
    WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	if (fh->fp == NULL)
		WT_RET_MSG(session, ENOTSUP,
		    "%s: vfprintf: no stream configured", fh->name);

	if (vfprintf(fh->fp, fmt, ap) >= 0)
		return (0);
	WT_RET_MSG(session, EIO, "%s: handle-printf: vfprintf", fh->name);
}

/*
 * __posix_handle_read --
 *	POSIX pread.
 */
static int
__posix_handle_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	size_t chunk;
	ssize_t nr;
	uint8_t *addr;

	/* Assert direct I/O is aligned and a multiple of the alignment. */
	WT_ASSERT(session,
	    !fh->direct_io ||
	    S2C(session)->buffer_alignment == 0 ||
	    (!((uintptr_t)buf &
	    (uintptr_t)(S2C(session)->buffer_alignment - 1)) &&
	    len >= S2C(session)->buffer_alignment &&
	    len % S2C(session)->buffer_alignment == 0));

	/* Break reads larger than 1GB into 1GB chunks. */
	for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
		chunk = WT_MIN(len, WT_GIGABYTE);
		if ((nr = pread(fh->fd, addr, chunk, offset)) <= 0)
			WT_RET_MSG(session, nr == 0 ? WT_ERROR : __wt_errno(),
			    "%s: handle-read: pread: failed to read %"
			    WT_SIZET_FMT " bytes at offset %" PRIuMAX,
			    fh->name, chunk, (uintmax_t)offset);
	}
	return (0);
}

/*
 * __posix_handle_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__posix_handle_size(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	struct stat sb;
	WT_DECL_RET;

	WT_SYSCALL_RETRY(fstat(fh->fd, &sb), ret);
	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}
	WT_RET_MSG(session, ret, "%s: handle-size: fstat", fh->name);
}

/*
 * __posix_handle_sync --
 *	POSIX fflush/fsync.
 */
static int
__posix_handle_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	if (fh->fp == NULL)
		return (__posix_sync(
		    session, fh->fd, fh->name, "handle-sync", block));

	if (fflush(fh->fp) == 0)
		return (0);
	WT_RET_MSG(session, __wt_errno(), "%s: handle-sync: fflush", fh->name);
}

/*
 * __posix_handle_truncate --
 *	POSIX ftruncate.
 */
static int
__posix_handle_truncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_DECL_RET;

	WT_SYSCALL_RETRY(ftruncate(fh->fd, len), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: handle-truncate: ftruncate", fh->name);
}

/*
 * __posix_handle_write --
 *	POSIX pwrite.
 */
static int
__posix_handle_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	size_t chunk;
	ssize_t nw;
	const uint8_t *addr;

	/* Assert direct I/O is aligned and a multiple of the alignment. */
	WT_ASSERT(session,
	    !fh->direct_io ||
	    S2C(session)->buffer_alignment == 0 ||
	    (!((uintptr_t)buf &
	    (uintptr_t)(S2C(session)->buffer_alignment - 1)) &&
	    len >= S2C(session)->buffer_alignment &&
	    len % S2C(session)->buffer_alignment == 0));

	/* Break writes larger than 1GB into 1GB chunks. */
	for (addr = buf; len > 0; addr += nw, len -= (size_t)nw, offset += nw) {
		chunk = WT_MIN(len, WT_GIGABYTE);
		if ((nw = pwrite(fh->fd, addr, chunk, offset)) < 0)
			WT_RET_MSG(session, __wt_errno(),
			    "%s: handle-write: pwrite: failed to write %"
			    WT_SIZET_FMT " bytes at offset %" PRIuMAX,
			    fh->name, chunk, (uintmax_t)offset);
	}
	return (0);
}

/*
 * __posix_handle_open_cloexec --
 *	Prevent child access to file handles.
 */
static inline int
__posix_handle_open_cloexec(WT_SESSION_IMPL *session, int fd, const char *name)
{
#if defined(HAVE_FCNTL) && defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
	int f;

	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles. There's an obvious race
	 * between the open and this call, prefer the flag to open if available.
	 */
	if ((f = fcntl(fd, F_GETFD)) == -1 ||
	    fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1)
		WT_RET_MSG(session, __wt_errno(),
		    "%s: handle-open: fcntl", name);
	return (0);
#else
	WT_UNUSED(session);
	WT_UNUSED(fd);
	WT_UNUSED(name);
	return (0);
#endif
}

/*
 * __posix_handle_open --
 *	Open a file handle.
 */
static int
__posix_handle_open(WT_SESSION_IMPL *session,
    WT_FH *fh, const char *name, uint32_t file_type, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	mode_t mode;
	int f, fd, tret;
	bool direct_io;
	const char *stream_mode;

	conn = S2C(session);
	direct_io = false;

	/* Set up error handling. */
	fh->fd = fd = -1;
	fh->fp = NULL;

	if (file_type == WT_FILE_TYPE_DIRECTORY) {
		f = O_RDONLY;
#ifdef O_CLOEXEC
		/*
		 * Security:
		 * The application may spawn a new process, and we don't want
		 * another process to have access to our file handles.
		 */
		f |= O_CLOEXEC;
#endif
		WT_SYSCALL_RETRY((
		    (fd = open(name, f, 0444)) == -1 ? 1 : 0), ret);
		if (ret != 0)
			WT_ERR_MSG(session, ret, "%s: handle-open: open", name);
		WT_ERR(__posix_handle_open_cloexec(session, fd, name));
		goto directory_open;
	}

	f = LF_ISSET(WT_OPEN_READONLY) ? O_RDONLY : O_RDWR;
	if (LF_ISSET(WT_OPEN_CREATE)) {
		f |= O_CREAT;
		if (LF_ISSET(WT_OPEN_EXCLUSIVE))
			f |= O_EXCL;
		mode = 0666;
	} else
		mode = 0;

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
#ifdef O_DIRECT
	/*
	 * Direct I/O: file-type is a flag from the set of possible flags stored
	 * in the connection handle during configuration, check for a match.
	 * Also, "direct_io=checkpoint" configures direct I/O for readonly data
	 * files.
	 */
	if (FLD_ISSET(conn->direct_io, file_type) ||
	    (LF_ISSET(WT_OPEN_READONLY) &&
	    file_type == WT_FILE_TYPE_DATA &&
	    FLD_ISSET(conn->direct_io, WT_FILE_TYPE_CHECKPOINT))) {
		f |= O_DIRECT;
		direct_io = true;
	}
#endif
	fh->direct_io = direct_io;
#ifdef O_NOATIME
	/* Avoid updating metadata for read-only workloads. */
	if (file_type == WT_FILE_TYPE_DATA)
		f |= O_NOATIME;
#endif

	if (file_type == WT_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC)) {
#ifdef O_DSYNC
		f |= O_DSYNC;
#elif defined(O_SYNC)
		f |= O_SYNC;
#else
		WT_ERR_MSG(session, ENOTSUP,
		    "unsupported log sync mode configured");
#endif
	}

	WT_SYSCALL_RETRY(((fd = open(name, f, mode)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret,
		    direct_io ?
		    "%s: handle-open: open: failed with direct I/O configured, "
		    "some filesystem types do not support direct I/O" :
		    "%s: handle-open: open", name);
	WT_ERR(__posix_handle_open_cloexec(session, fd, name));

	/* Disable read-ahead on trees: it slows down random read workloads. */
#if defined(HAVE_POSIX_FADVISE)
	if (file_type == WT_FILE_TYPE_DATA) {
		WT_SYSCALL_RETRY(
		    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM), ret);
		if (ret != 0)
			WT_ERR_MSG(session, ret,
			    "%s: handle-open: posix_fadvise", name);
	}
#endif

	/* Optionally configure a stdio stream API. */
	switch (LF_MASK(WT_STREAM_APPEND | WT_STREAM_READ | WT_STREAM_WRITE)) {
	case WT_STREAM_APPEND:
		stream_mode = "a";
		F_SET(fh, WT_FH_FLUSH_ON_CLOSE);
		break;
	case WT_STREAM_READ:
		stream_mode = "r";
		break;
	case WT_STREAM_WRITE:
		stream_mode = "w";
		F_SET(fh, WT_FH_FLUSH_ON_CLOSE);
		break;
	case 0:
	default:
		stream_mode = NULL;
		break;
	}
	if (stream_mode != NULL) {
		if ((fh->fp = fdopen(fd, stream_mode)) == NULL)
			WT_ERR_MSG(session, __wt_errno(),
			    "%s: handle-open: fdopen", name);
		if (LF_ISSET(WT_STREAM_LINE_BUFFER))
			__wt_stream_set_line_buffer(fh->fp);
	}

directory_open:
	fh->fd = fd;

	/* Configure fallocate calls. */
	__wt_posix_handle_allocate_configure(session, fh);

	fh->fh_advise = __posix_handle_advise;
	fh->fh_allocate = __wt_posix_handle_allocate;
	fh->fh_close = __posix_handle_close;
	fh->fh_getc = __posix_handle_getc;
	fh->fh_lock = __posix_handle_lock;
	fh->fh_map = __wt_posix_map;
	fh->fh_map_discard = __wt_posix_map_discard;
	fh->fh_map_preload = __wt_posix_map_preload;
	fh->fh_map_unmap = __wt_posix_map_unmap;
	fh->fh_printf = __posix_handle_printf;
	fh->fh_read = __posix_handle_read;
	fh->fh_size = __posix_handle_size;
	fh->fh_sync = __posix_handle_sync;
	fh->fh_truncate = __posix_handle_truncate;
	fh->fh_write = __posix_handle_write;

	return (0);

err:	if (fd != -1) {
		WT_SYSCALL_RETRY(close(fd), tret);
		if (tret != 0)
			__wt_err(session, tret, "%s: handle-open: close", name);
	}
	return (ret);
}

/*
 * __wt_os_posix --
 *	Initialize a POSIX configuration.
 */
int
__wt_os_posix(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* Initialize the POSIX jump table. */
	conn->file_directory_list = __wt_posix_directory_list;
	conn->file_directory_sync = __posix_directory_sync;
	conn->file_exist = __posix_file_exist;
	conn->file_remove = __posix_file_remove;
	conn->file_rename = __posix_file_rename;
	conn->file_size = __posix_file_size;
	conn->handle_open = __posix_handle_open;

	return (0);
}

/*
 * __wt_os_posix_cleanup --
 *	Discard a POSIX configuration.
 */
int
__wt_os_posix_cleanup(WT_SESSION_IMPL *session)
{
	WT_UNUSED(session);

	return (0);
}
