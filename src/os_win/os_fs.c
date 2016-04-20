/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __win_directory_sync --
 *	Flush a directory to ensure a file creation is durable.
 */
static int
__win_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
	WT_UNUSED(session);
	WT_UNUSED(path);
	return (0);
}

/*
 * __win_file_exist --
 *	Return if the file exists.
 */
static int
__win_file_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, name, &path));

	ret = GetFileAttributesA(path);

	__wt_free(session, path);

	if (ret != INVALID_FILE_ATTRIBUTES)
		*existp = true;
	else
		*existp = false;

	return (0);
}

/*
 * __win_file_remove --
 *	Remove a file.
 */
static int
__win_file_remove(WT_SESSION_IMPL *session, const char *name)
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

	if (DeleteFileA(name) == FALSE) {
		ret = __wt_getlasterror();
		__wt_err(session, ret, "%s: file-remove: DeleteFileA", name);
	}

	__wt_free(session, path);
	return (ret);
}

/*
 * __win_file_rename --
 *	Rename a file.
 */
static int
__win_file_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
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

	/*
	 * Check if file exists since Windows does not override the file if
	 * it exists.
	 */
	if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES)
		if (DeleteFileA(to) == FALSE) {
			ret = __wt_getlasterror();
			__wt_err(session, ret,
			    "%s to %s: file-rename: rename", from, to);
		}

	if (ret == 0 && MoveFileA(from, to) == FALSE) {
		ret = __wt_getlasterror();
		__wt_err(session, ret,
		    "%s to %s: file-rename: rename", from, to);
	}

err:	__wt_free(session, from_path);
	__wt_free(session, to_path);
	return (ret);
}

/*
 * __win_file_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
__win_file_size(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, name, &path));

	ret = GetFileAttributesExA(path, GetFileExInfoStandard, &data);

	__wt_free(session, path);

	if (ret != 0) {
		*sizep =
		    ((int64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
		return (0);
	}

	/*
	 * Some callers of this function expect failure if the file doesn't
	 * exist, and don't want an error message logged.
	 */
	ret = __wt_getlasterror();
	if (!silent)
		WT_RET_MSG(session, ret,
		    "%s: file-size: GetFileAttributesEx", name);
	return (ret);
}

/*
 * __win_handle_allocate_configure --
 *	Configure fallocate behavior for a file handle.
 */
static void
__win_handle_allocate_configure(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);

	/*
	 * fallocate on Windows would be implemented using SetEndOfFile, which
	 * can also truncate the file. WiredTiger expects fallocate to ignore
	 * requests to truncate the file which Windows does not do, so we don't
	 * support the call.
	 */
	fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
	fh->fallocate_requires_locking = false;
}

/*
 * __win_handle_close --
 *	ANSI C close.
 */
static int
__win_handle_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_DECL_RET;

	/*
	 * Close the primary and secondary handles.
	 *
	 * We don't open Windows system handles when opening directories for
	 * flushing, as it's not necessary (or possible) to flush a directory
	 * on Windows. Confirm the file handle is open before closing it.
	 */
	if (fh->filehandle != INVALID_HANDLE_VALUE &&
	    CloseHandle(fh->filehandle) == 0) {
		ret = __wt_getlasterror();
		__wt_err(session, ret,
		    "%s: handle-close: CloseHandle", fh->name);
	}

	if (fh->filehandle_secondary != INVALID_HANDLE_VALUE &&
	    CloseHandle(fh->filehandle_secondary) == 0) {
		ret = __wt_getlasterror();
		__wt_err(session, ret,
		    "%s: handle-close: secondary: CloseHandle", fh->name);
	}

	return (ret);
}

/*
 * __win_handle_lock --
 *	Lock/unlock a file.
 */
static int
__win_handle_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	WT_DECL_RET;

	/*
	 * WiredTiger requires this function be able to acquire locks past
	 * the end of file.
	 *
	 * Note we're using fcntl(2) locking: all fcntl locks associated with a
	 * file for a given process are removed when any file descriptor for the
	 * file is closed by the process, even if a lock was never requested for
	 * that file descriptor.
	 *
	 * http://msdn.microsoft.com/
	 *    en-us/library/windows/desktop/aa365202%28v=vs.85%29.aspx
	 *
	 * You can lock bytes that are beyond the end of the current file.
	 * This is useful to coordinate adding records to the end of a file.
	 */
	if (lock) {
		if (LockFile(fh->filehandle, 0, 0, 1, 0) == FALSE) {
			ret = __wt_getlasterror();
			__wt_err(session, ret,
			    "%s: handle-lock: LockFile", fh->name);
		}
	} else
		if (UnlockFile(fh->filehandle, 0, 0, 1, 0) == FALSE) {
			ret = __wt_getlasterror();
			__wt_err(session, ret,
			    "%s: handle-lock: UnlockFile", fh->name);
		}
	return (ret);
}

/*
 * __win_handle_read --
 *	Read a chunk.
 */
static int
__win_handle_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	DWORD chunk, nr;
	uint8_t *addr;
	OVERLAPPED overlapped = { 0 };

	nr = 0;

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
		chunk = (DWORD)WT_MIN(len, WT_GIGABYTE);
		overlapped.Offset = UINT32_MAX & offset;
		overlapped.OffsetHigh = UINT32_MAX & (offset >> 32);

		if (!ReadFile(fh->filehandle, addr, chunk, &nr, &overlapped))
			WT_RET_MSG(session,
			    nr == 0 ? WT_ERROR : __wt_getlasterror(),
			    "%s: handle-read: ReadFile: failed to read %lu "
			    "bytes at offset %" PRIuMAX,
			    fh->name, chunk, (uintmax_t)offset);
	}
	return (0);
}

/*
 * __win_handle_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__win_handle_size(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	LARGE_INTEGER size;

	if (GetFileSizeEx(fh->filehandle, &size) != 0) {
		*sizep = size.QuadPart;
		return (0);
	}

	WT_RET_MSG(session,
	    __wt_getlasterror(), "%s: handle-size: GetFileSizeEx", fh->name);
}

/*
 * __win_handle_sync --
 *	MSVC fsync.
 */
static int
__win_handle_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_DECL_RET;

	/*
	 * We don't open Windows system handles when opening directories
	 * for flushing, as it is not necessary (or possible) to flush
	 * a directory on Windows. Confirm the file handle is set before
	 * attempting to sync it.
	 */
	if (fh->filehandle == INVALID_HANDLE_VALUE)
		return (0);

	/*
	 * Callers attempting asynchronous flush handle ENOTSUP returns,
	 * and won't make further attempts.
	 */
	if (!block)
		return (ENOTSUP);

	if (FlushFileBuffers(fh->filehandle) == FALSE) {
		ret = __wt_getlasterror();
		WT_RET_MSG(session, ret,
		    "%s handle-sync: FlushFileBuffers error", fh->name);
	}
	return (0);
}

/*
 * __win_handle_truncate --
 *	Truncate a file.
 */
static int
__win_handle_truncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_DECL_RET;
	LARGE_INTEGER largeint;

	largeint.QuadPart = len;

	if (fh->filehandle_secondary == INVALID_HANDLE_VALUE)
		WT_RET_MSG(session, EINVAL,
		    "%s: handle-truncate: read-only", fh->name);

	if (SetFilePointerEx(
	    fh->filehandle_secondary, largeint, NULL, FILE_BEGIN) == FALSE)
		WT_RET_MSG(session, __wt_getlasterror(),
		    "%s: handle-truncate: SetFilePointerEx", fh->name);

	if (SetEndOfFile(fh->filehandle_secondary) == FALSE) {
		if (GetLastError() == ERROR_USER_MAPPED_FILE)
			return (EBUSY);
		WT_RET_MSG(session, __wt_getlasterror(),
		    "%s: handle-truncate: SetEndOfFile error", fh->name);
	}
	return (0);
}

/*
 * __win_handle_write --
 *	Write a chunk.
 */
static int
__win_handle_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	DWORD chunk;
	DWORD nw;
	const uint8_t *addr;
	OVERLAPPED overlapped = { 0 };

	nw = 0;

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
		chunk = (DWORD)WT_MIN(len, WT_GIGABYTE);
		overlapped.Offset = UINT32_MAX & offset;
		overlapped.OffsetHigh = UINT32_MAX & (offset >> 32);

		if (!WriteFile(fh->filehandle, addr, chunk, &nw, &overlapped))
			WT_RET_MSG(session, __wt_getlasterror(),
			    "%s: handle-write: WriteFile: failed to write %lu "
			    "bytes at offset %" PRIuMAX,
			    fh->name, chunk, (uintmax_t)offset);
	}
	return (0);
}

/*
 * __win_file_open --
 *	Open a file handle.
 */
static int
__win_file_open(WT_SESSION_IMPL *session,
    WT_FH *fh, const char *name, uint32_t file_type, uint32_t flags)
{
	DWORD dwCreationDisposition;
	HANDLE filehandle, filehandle_secondary;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int desired_access, f;
	bool direct_io;

	conn = S2C(session);
	direct_io = false;

	/* Set up error handling. */
	fh->filehandle = fh->filehandle_secondary =
	    filehandle = filehandle_secondary = INVALID_HANDLE_VALUE;

	/*
	 * Opening a file handle on a directory is only to support filesystems
	 * that require a directory sync for durability, and Windows doesn't
	 * require that functionality: create an empty WT_FH structure with
	 * invalid handles.
	 */
	if (file_type == WT_FILE_TYPE_DIRECTORY)
		goto directory_open;

	desired_access = GENERIC_READ;
	if (!LF_ISSET(WT_OPEN_READONLY))
		desired_access |= GENERIC_WRITE;

	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles.
	 *
	 * TODO: Set tighter file permissions but set bInheritHandle to false
	 * to prevent inheritance
	 */
	f = FILE_ATTRIBUTE_NORMAL;

	dwCreationDisposition = 0;
	if (LF_ISSET(WT_OPEN_CREATE)) {
		dwCreationDisposition = CREATE_NEW;
		if (LF_ISSET(WT_OPEN_EXCLUSIVE))
			dwCreationDisposition = CREATE_ALWAYS;
	} else
		dwCreationDisposition = OPEN_EXISTING;

	/* Direct I/O. */
	if (LF_ISSET(WT_OPEN_DIRECTIO)) {
		f |= FILE_FLAG_NO_BUFFERING;
		fh->direct_io = true;
	}

	/* FILE_FLAG_WRITE_THROUGH does not require aligned buffers */
	if (FLD_ISSET(conn->write_through, file_type))
		f |= FILE_FLAG_WRITE_THROUGH;

	if (file_type == WT_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC))
		f |= FILE_FLAG_WRITE_THROUGH;

	/* Disable read-ahead on trees: it slows down random read workloads. */
	if (file_type == WT_FILE_TYPE_DATA)
		f |= FILE_FLAG_RANDOM_ACCESS;

	filehandle = CreateFileA(name, desired_access,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    NULL, dwCreationDisposition, f, NULL);
	if (filehandle == INVALID_HANDLE_VALUE) {
		if (LF_ISSET(WT_OPEN_CREATE) &&
		    GetLastError() == ERROR_FILE_EXISTS)
			filehandle = CreateFileA(name, desired_access,
			    FILE_SHARE_READ | FILE_SHARE_WRITE,
			    NULL, OPEN_EXISTING, f, NULL);
		if (filehandle == INVALID_HANDLE_VALUE)
			WT_ERR_MSG(session, __wt_getlasterror(),
			    direct_io ?
			    "%s: handle-open: CreateFileA: failed with direct "
			    "I/O configured, some filesystem types do not "
			    "support direct I/O" :
			    "%s: handle-open: CreateFileA", name);
	}

	/*
	 * Open a second handle to file to support allocation/truncation
	 * concurrently with reads on the file. Writes would also move the file
	 * pointer.
	 */
	if (!LF_ISSET(WT_OPEN_READONLY)) {
		filehandle_secondary = CreateFileA(name, desired_access,
		    FILE_SHARE_READ | FILE_SHARE_WRITE,
		    NULL, OPEN_EXISTING, f, NULL);
		if (filehandle_secondary == INVALID_HANDLE_VALUE)
			WT_ERR_MSG(session, __wt_getlasterror(),
			    "%s: handle-open: CreateFileA: secondary", name);
	}

	/* Configure fallocate/posix_fallocate calls. */
	__win_handle_allocate_configure(session, fh);

directory_open:
	fh->filehandle = filehandle;
	fh->filehandle_secondary = filehandle_secondary;

	fh->fh_close = __win_handle_close;
	fh->fh_lock = __win_handle_lock;
	fh->fh_map = __wt_win_map;
	fh->fh_map_discard = __wt_win_map_discard;
	fh->fh_map_preload = __wt_win_map_preload;
	fh->fh_map_unmap = __wt_win_map_unmap;
	fh->fh_read = __win_handle_read;
	fh->fh_size = __win_handle_size;
	fh->fh_sync = __win_handle_sync;
	fh->fh_truncate = __win_handle_truncate;
	fh->fh_write = __win_handle_write;

	return (0);

err:	if (filehandle != INVALID_HANDLE_VALUE)
		(void)CloseHandle(filehandle);
	if (filehandle_secondary != INVALID_HANDLE_VALUE)
		(void)CloseHandle(filehandle_secondary);

	return (ret);
}

/*
 * __wt_os_win --
 *	Initialize a MSVC configuration.
 */
int
__wt_os_win(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* Initialize the POSIX jump table. */
	conn->file_directory_list = __wt_win_directory_list;
	conn->file_directory_sync = __win_directory_sync;
	conn->file_exist = __win_file_exist;
	conn->file_open = __win_file_open;
	conn->file_remove = __win_file_remove;
	conn->file_rename = __win_file_rename;
	conn->file_size = __win_file_size;

	return (0);
}

/*
 * __wt_os_win_cleanup --
 *	Discard a POSIX configuration.
 */
int
__wt_os_win_cleanup(WT_SESSION_IMPL *session)
{
	WT_UNUSED(session);

	return (0);
}
