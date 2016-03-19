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
__posix_sync(WT_SESSION_IMPL *session, int fd, const char *name, bool block)
{
	WT_DECL_RET;

#ifdef	HAVE_SYNC_FILE_RANGE
	if (!block) {
		WT_SYSCALL_RETRY(sync_file_range(fd,
		    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE), ret);
		if (ret == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: sync_file_range", name);
	}
#else
	if (!block)
		return (0);
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
	WT_RET_MSG(session, ret, "%s: fdatasync", name);
#else
	WT_SYSCALL_RETRY(fsync(fd), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: fsync", name);
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
		WT_RET_MSG(session, ret, "%s: open", path);

	ret = __posix_sync(session, fd, path, true);

	WT_SYSCALL_RETRY(close(fd), tret);
	if (tret != 0)
		__wt_err(session, tret, "%s: fsync", path);
	return (ret == 0 ? tret : ret);
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
		__wt_err(session, ret, "%s: stat", name);

	__wt_free(session, path);
	return (ret);
}

/*
 * __posix_file_remove --
 *	POSIX remove.
 */
static int
__posix_file_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;
	char *path;

#ifdef HAVE_DIAGNOSTIC
	if (__wt_handle_search(session, name, false, true, NULL, NULL))
		WT_RET_MSG(
		    session, EINVAL, "%s: remove: file has open handles", name);
#endif

	WT_RET(__wt_filename(session, name, &path));
	name = path;

	WT_SYSCALL_RETRY(remove(name), ret);
	if (ret != 0)
		__wt_err(session, ret, "%s: remove", path);

	__wt_free(session, path);
	return (ret);
}

/*
 * __posix_file_rename --
 *	POSIX rename.
 */
static int
__posix_file_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_DECL_RET;
	char *from_path, *to_path;

#ifdef HAVE_DIAGNOSTIC
	if (__wt_handle_search(session, from, false, true, NULL, NULL))
		WT_RET_MSG(
		    session, EINVAL, "%s: rename: file has open handles", from);
	if (__wt_handle_search(session, to, false, true, NULL, NULL))
		WT_RET_MSG(
		    session, EINVAL, "%s: rename: file has open handles", to);
#endif

	from_path = to_path = NULL;
	WT_ERR(__wt_filename(session, from, &from_path));
	from = from_path;
	WT_ERR(__wt_filename(session, to, &to_path));
	to = to_path;

	WT_SYSCALL_RETRY(rename(from, to), ret);
	if (ret != 0)
		__wt_err(session, ret, "%s to %s: rename", from, to);

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
		__wt_err(session, ret, "%s: stat", name);

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

	WT_SYSCALL_RETRY(posix_fadvise(fh->fd, offset, len, advice), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: posix_fadvise", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(advice);
	return (0);
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
	int tret;

	if (fh->fp == NULL) {
		WT_SYSCALL_RETRY(close(fh->fd), ret);
		if (ret == 0)
			return (0);
		WT_RET_MSG(session, ret, "%s: close", fh->name);
	}

	/* If the handle was opened for writing, flush the file. */
	if (F_ISSET(fh, WT_FH_FLUSH_ON_CLOSE) && fflush(fh->fp) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "%s: fflush", fh->name);
	}

	if ((tret = fclose(fh->fp)) != 0) {
		tret = __wt_errno();
		__wt_err(session, tret, "%s: fclose", fh->name);
	}
	return (ret == 0 ? tret : ret);
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
		    ENOTSUP, "%s: getc: no stream configured", fh->name);

	*chp = fgetc(fh->fp);
	if (*chp != EOF || !ferror(fh->fp))
		return (0);
	WT_RET_MSG(session, __wt_errno(), "%s: getc", fh->name);
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
	WT_RET_MSG(session, ret, "%s: fcntl", fh->name);
}

/*
 * __posix_handle_open --
 *	POSIX fopen/open.
 */
static int
__posix_handle_open(WT_SESSION_IMPL *session,
    WT_FH *fh, const char *name, int dio_type, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	mode_t mode;
	int f, fd, tret;
	bool direct_io;
	char *path;
	const char *stream_mode;

	conn = S2C(session);
	direct_io = false;

	/* 0 is a legal file descriptor, set up error handling. */
	fh->fd = fd = -1;

	/* Create the path to the file. */
	path = NULL;
	if (!LF_ISSET(WT_OPEN_FIXED)) {
		WT_ERR(__wt_filename(session, name, &path));
		name = path;
	}

	if (dio_type == WT_FILE_TYPE_DIRECTORY) {
		WT_SYSCALL_RETRY((
		    (fd = open(name, O_RDONLY, 0444)) == -1 ? 1 : 0), ret);
		if (ret == 0)
			goto setupfh;
		WT_ERR_MSG(session, ret, "%s: open", name);
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
	if (dio_type && FLD_ISSET(conn->direct_io, dio_type)) {
		f |= O_DIRECT;
		direct_io = true;
	}
#endif
	fh->direct_io = direct_io;
#ifdef O_NOATIME
	/* Avoid updating metadata for read-only workloads. */
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
		f |= O_NOATIME;
#endif

	if (dio_type == WT_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC)) {
#ifdef O_DSYNC
		f |= O_DSYNC;
#elif defined(O_SYNC)
		f |= O_SYNC;
#else
		WT_ERR_MSG(session, ENOTSUP,
		    "Unsupported log sync mode requested");
#endif
	}

	WT_SYSCALL_RETRY(((fd = open(name, f, mode)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret,
		    direct_io ?
		    "%s: open failed with direct I/O configured, some "
		    "filesystem types do not support direct I/O" : "%s", name);

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

	/* Disable read-ahead on trees: it slows down random read workloads. */
#if defined(HAVE_POSIX_FADVISE)
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT) {
		WT_SYSCALL_RETRY(
		    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM), ret);
		if (ret != 0)
			WT_ERR_MSG(session, ret, "%s: posix_fadvise", name);
	}
#endif

	/* Configure file extension. */
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
		fh->extend_len = conn->data_extend_len;

	/* Optionally configure the stream API. */
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
	if (stream_mode != NULL && (fh->fp = fdopen(fd, stream_mode)) == NULL)
		WT_ERR_MSG(session, __wt_errno(), "%s: fopen", name);

	__wt_free(session, path);
	fh->fd = fd;
	return (0);

err:	if (fd != -1) {
		WT_SYSCALL_RETRY(close(fd), tret);
		if (tret != 0)
			__wt_err(session, tret, "%s: close", name);
	}
	__wt_free(session, path);
	fh->fd = -1;
	fh->fp = NULL;
	return (ret);
}

/*
 * __posix_handle_printf --
 *	ANSI C vfprintf.
 */
static int
__posix_handle_printf(
    WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	if (fh == WT_STDERR || fh == WT_STDOUT) {
		if (vfprintf(fh == WT_STDERR ? stderr : stdout, fmt, ap) >= 0)
			return (0);
		WT_RET_MSG(session, EIO,
		    "%s: vfprintf", fh == WT_STDERR ? "stderr" : "stdout");
	}

	if (fh->fp == NULL)
		WT_RET_MSG(session, ENOTSUP,
		    "%s: vfprintf: no stream configured", fh->name);

	if (vfprintf(fh->fp, fmt, ap) >= 0)
		return (0);
	WT_RET_MSG(session, EIO, "%s: vfprintf", fh->name);
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
			    "%s read error: failed to read %" WT_SIZET_FMT
			    " bytes at offset %" PRIuMAX,
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
	WT_RET_MSG(session, ret, "%s: fstat", fh->name);
}

/*
 * __posix_handle_sync --
 *	POSIX fflush/fsync.
 */
static int
__posix_handle_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	/* Flush any stream's stdio buffers. */
	if (fh == WT_STDERR || fh == WT_STDOUT) {
		if (fflush(fh == WT_STDERR ? stderr : stdout) == 0)
			return (0);
		WT_RET_MSG(session, __wt_errno(),
		    "%s: fflush", fh == WT_STDERR ? "stderr" : "stdout");
	}

	if (fh->fp == NULL)
		return (__posix_sync(session, fh->fd, fh->name, block));

	if (fflush(fh->fp) == 0)
		return (0);
	WT_RET_MSG(session, __wt_errno(), "%s: fflush", fh->name);
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
	WT_RET_MSG(session, ret, "%s: ftruncate", fh->name);
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
			    "%s write error: failed to write %" WT_SIZET_FMT
			    " bytes at offset %" PRIuMAX,
			    fh->name, chunk, (uintmax_t)offset);
	}
	return (0);
}

/*
 * __wt_os_posix --
 *	Initialize a POSIX configuration.
 */
int
__wt_os_posix(WT_SESSION_IMPL *session)
{
	WT_UNUSED(session);

	/* Initialize the POSIX jump table. */
	__wt_process.j_directory_sync = __posix_directory_sync;
	__wt_process.j_file_exist = __posix_file_exist;
	__wt_process.j_file_remove = __posix_file_remove;
	__wt_process.j_file_rename = __posix_file_rename;
	__wt_process.j_file_size = __posix_file_size;
	__wt_process.j_handle_advise = __posix_handle_advise;
	__wt_process.j_handle_close = __posix_handle_close;
	__wt_process.j_handle_getc = __posix_handle_getc;
	__wt_process.j_handle_lock = __posix_handle_lock;
	__wt_process.j_handle_open = __posix_handle_open;
	__wt_process.j_handle_printf = __posix_handle_printf;
	__wt_process.j_handle_read = __posix_handle_read;
	__wt_process.j_handle_size = __posix_handle_size;
	__wt_process.j_handle_sync = __posix_handle_sync;
	__wt_process.j_handle_truncate = __posix_handle_truncate;
	__wt_process.j_handle_write = __posix_handle_write;

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
