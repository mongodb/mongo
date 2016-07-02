/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __win_fs_exist --
 *	Return if the file exists.
 */
static int
__win_fs_exist(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *name, bool *existp)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	if (GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES)
		*existp = true;
	else
		*existp = false;

	return (0);
}

/*
 * __win_fs_remove --
 *	Remove a file.
 */
static int
__win_fs_remove(
    WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name)
{
	DWORD windows_error;
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	if (DeleteFileA(name) == FALSE) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: file-remove: DeleteFileA: %s",
		    name, __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}
	return (0);
}

/*
 * __win_fs_rename --
 *	Rename a file.
 */
static int
__win_fs_rename(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *from, const char *to)
{
	DWORD windows_error;
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	/*
	 * Check if file exists since Windows does not override the file if
	 * it exists.
	 */
	if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES)
		if (DeleteFileA(to) == FALSE) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: file-rename: DeleteFileA: %s",
			    to, __wt_formatmessage(session, windows_error));
			return (__wt_map_windows_error(windows_error));
		}

	if (MoveFileA(from, to) == FALSE) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s to %s: file-rename: MoveFileA: %s",
		    from, to, __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}

	return (0);
}

/*
 * __wt_win_fs_size --
 *	Get the size of a file in bytes, by file name.
 */
int
__wt_win_fs_size(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *name, wt_off_t *sizep)
{
	DWORD windows_error;
	WIN32_FILE_ATTRIBUTE_DATA data;
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	if (GetFileAttributesExA(name, GetFileExInfoStandard, &data) != 0) {
		*sizep =
		    ((int64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
		return (0);
	}

	windows_error = __wt_getlasterror();
	__wt_errx(session,
	    "%s: file-size: GetFileAttributesEx: %s",
	    name, __wt_formatmessage(session, windows_error));
	return (__wt_map_windows_error(windows_error));
}

/*
 * __win_file_close --
 *	ANSI C close.
 */
static int
__win_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
	DWORD windows_error;
	WT_DECL_RET;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	/*
	 * Close the primary and secondary handles.
	 *
	 * We don't open Windows system handles when opening directories for
	 * flushing, as it's not necessary (or possible) to flush a directory
	 * on Windows. Confirm the file handle is open before closing it.
	 */
	if (win_fh->filehandle != INVALID_HANDLE_VALUE &&
	    CloseHandle(win_fh->filehandle) == 0) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: handle-close: CloseHandle: %s",
		    file_handle->name,
		    __wt_formatmessage(session, windows_error));
		ret = __wt_map_windows_error(windows_error);
	}

	if (win_fh->filehandle_secondary != INVALID_HANDLE_VALUE &&
	    CloseHandle(win_fh->filehandle_secondary) == 0) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: handle-close: secondary: CloseHandle: %s",
		    file_handle->name,
		    __wt_formatmessage(session, windows_error));
		ret = __wt_map_windows_error(windows_error);
	}

	__wt_free(session, file_handle->name);
	__wt_free(session, win_fh);
	return (ret);
}

/*
 * __win_file_lock --
 *	Lock/unlock a file.
 */
static int
__win_file_lock(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, bool lock)
{
	DWORD windows_error;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	/*
	 * WiredTiger requires this function be able to acquire locks past
	 * the end of file.
	 *
	 * http://msdn.microsoft.com/
	 *    en-us/library/windows/desktop/aa365202%28v=vs.85%29.aspx
	 *
	 * You can lock bytes that are beyond the end of the current file.
	 * This is useful to coordinate adding records to the end of a file.
	 */
	if (lock) {
		if (LockFile(win_fh->filehandle, 0, 0, 1, 0) == FALSE) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: handle-lock: LockFile: %s",
			    file_handle->name,
			    __wt_formatmessage(session, windows_error));
			return (__wt_map_windows_error(windows_error));
		}
	} else
		if (UnlockFile(win_fh->filehandle, 0, 0, 1, 0) == FALSE) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: handle-lock: UnlockFile: %s",
			    file_handle->name,
			    __wt_formatmessage(session, windows_error));
			return (__wt_map_windows_error(windows_error));
		}
	return (0);
}

/*
 * __win_file_read --
 *	Read a chunk.
 */
static int
__win_file_read(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
	DWORD chunk, nr, windows_error;
	uint8_t *addr;
	OVERLAPPED overlapped = { 0 };
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	nr = 0;

	/* Assert direct I/O is aligned and a multiple of the alignment. */
	WT_ASSERT(session,
	    !win_fh->direct_io ||
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

		if (!ReadFile(
		    win_fh->filehandle, addr, chunk, &nr, &overlapped)) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: handle-read: ReadFile: failed to read %lu "
			    "bytes at offset %" PRIuMAX ": %s",
			    file_handle->name, chunk, (uintmax_t)offset,
			    __wt_formatmessage(session, windows_error));
			return (__wt_map_windows_error(windows_error));
		}
	}
	return (0);
}

/*
 * __win_file_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__win_file_size(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t *sizep)
{
	DWORD windows_error;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;
	LARGE_INTEGER size;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	if (GetFileSizeEx(win_fh->filehandle, &size) != 0) {
		*sizep = size.QuadPart;
		return (0);
	}

	windows_error = __wt_getlasterror();
	__wt_errx(session,
	    "%s: handle-size: GetFileSizeEx: %s",
	    file_handle->name, __wt_formatmessage(session, windows_error));
	return (__wt_map_windows_error(windows_error));
}

/*
 * __win_file_sync --
 *	MSVC fsync.
 */
static int
__win_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
	DWORD windows_error;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	/*
	 * We don't open Windows system handles when opening directories
	 * for flushing, as it is not necessary (or possible) to flush
	 * a directory on Windows. Confirm the file handle is set before
	 * attempting to sync it.
	 */
	if (win_fh->filehandle == INVALID_HANDLE_VALUE)
		return (0);

	if (FlushFileBuffers(win_fh->filehandle) == FALSE) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s handle-sync: FlushFileBuffers: %s",
		    file_handle->name,
		    __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}
	return (0);
}

/*
 * __win_file_truncate --
 *	Truncate a file.
 */
static int
__win_file_truncate(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t len)
{
	DWORD windows_error;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;
	LARGE_INTEGER largeint;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	largeint.QuadPart = len;

	if (win_fh->filehandle_secondary == INVALID_HANDLE_VALUE)
		WT_RET_MSG(session, EINVAL,
		    "%s: handle-truncate: read-only", file_handle->name);

	if (SetFilePointerEx(win_fh->filehandle_secondary,
	    largeint, NULL, FILE_BEGIN) == FALSE) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: handle-truncate: SetFilePointerEx: %s",
		    file_handle->name,
		    __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}

	if (SetEndOfFile(win_fh->filehandle_secondary) == FALSE) {
		if (GetLastError() == ERROR_USER_MAPPED_FILE)
			return (EBUSY);
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: handle-truncate: SetEndOfFile: %s",
		    file_handle->name,
		    __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}
	return (0);
}

/*
 * __win_file_write --
 *	Write a chunk.
 */
static int
__win_file_write(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
	DWORD chunk, nw, windows_error;
	const uint8_t *addr;
	OVERLAPPED overlapped = { 0 };
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;

	win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
	session = (WT_SESSION_IMPL *)wt_session;

	nw = 0;

	/* Assert direct I/O is aligned and a multiple of the alignment. */
	WT_ASSERT(session,
	    !win_fh->direct_io ||
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

		if (!WriteFile(
		    win_fh->filehandle, addr, chunk, &nw, &overlapped)) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: handle-write: WriteFile: failed to write %lu "
			    "bytes at offset %" PRIuMAX ": %s",
			    file_handle->name, chunk, (uintmax_t)offset,
			    __wt_formatmessage(session, windows_error));
			return (__wt_map_windows_error(windows_error));
		}
	}
	return (0);
}

/*
 * __win_open_file --
 *	Open a file handle.
 */
static int
__win_open_file(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
    const char *name, WT_OPEN_FILE_TYPE file_type, uint32_t flags,
    WT_FILE_HANDLE **file_handlep)
{
	DWORD dwCreationDisposition, windows_error;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FILE_HANDLE *file_handle;
	WT_FILE_HANDLE_WIN *win_fh;
	WT_SESSION_IMPL *session;
	int desired_access, f;

	WT_UNUSED(file_system);

	*file_handlep = NULL;

	session = (WT_SESSION_IMPL *)wt_session;
	conn = S2C(session);

	WT_RET(__wt_calloc_one(session, &win_fh));

	win_fh->direct_io = false;

	/* Set up error handling. */
	win_fh->filehandle =
	    win_fh->filehandle_secondary = INVALID_HANDLE_VALUE;

	/*
	 * Opening a file handle on a directory is only to support filesystems
	 * that require a directory sync for durability, and Windows doesn't
	 * require that functionality: create an empty WT_FH structure with
	 * invalid handles.
	 */
	if (file_type == WT_OPEN_FILE_TYPE_DIRECTORY)
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
		win_fh->direct_io = true;
	}

	/* FILE_FLAG_WRITE_THROUGH does not require aligned buffers */
	if (FLD_ISSET(conn->write_through, file_type))
		f |= FILE_FLAG_WRITE_THROUGH;

	if (file_type == WT_OPEN_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC))
		f |= FILE_FLAG_WRITE_THROUGH;

	/* Disable read-ahead on trees: it slows down random read workloads. */
	if (file_type == WT_OPEN_FILE_TYPE_DATA)
		f |= FILE_FLAG_RANDOM_ACCESS;

	win_fh->filehandle = CreateFileA(name, desired_access,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    NULL, dwCreationDisposition, f, NULL);
	if (win_fh->filehandle == INVALID_HANDLE_VALUE) {
		if (LF_ISSET(WT_OPEN_CREATE) &&
		    GetLastError() == ERROR_FILE_EXISTS)
			win_fh->filehandle = CreateFileA(name, desired_access,
			    FILE_SHARE_READ | FILE_SHARE_WRITE,
			    NULL, OPEN_EXISTING, f, NULL);
		if (win_fh->filehandle == INVALID_HANDLE_VALUE) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    win_fh->direct_io ?
			    "%s: handle-open: CreateFileA: failed with direct "
			    "I/O configured, some filesystem types do not "
			    "support direct I/O: %s" :
			    "%s: handle-open: CreateFileA: %s",
			    name, __wt_formatmessage(session, windows_error));
			WT_ERR(__wt_map_windows_error(windows_error));
		}
	}

	/*
	 * Open a second handle to file to support allocation/truncation
	 * concurrently with reads on the file. Writes would also move the file
	 * pointer.
	 */
	if (!LF_ISSET(WT_OPEN_READONLY)) {
		win_fh->filehandle_secondary = CreateFileA(name, desired_access,
		    FILE_SHARE_READ | FILE_SHARE_WRITE,
		    NULL, OPEN_EXISTING, f, NULL);
		if (win_fh->filehandle_secondary == INVALID_HANDLE_VALUE) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: handle-open: CreateFileA: secondary: %s",
			    name, __wt_formatmessage(session, windows_error));
			WT_ERR(__wt_map_windows_error(windows_error));
		}
	}

directory_open:
	/* Initialize public information. */
	file_handle = (WT_FILE_HANDLE *)win_fh;
	WT_ERR(__wt_strdup(session, name, &file_handle->name));

	file_handle->close = __win_file_close;
	file_handle->fh_lock = __win_file_lock;
#ifdef WORDS_BIGENDIAN
	/*
	 * The underlying objects are little-endian, mapping objects isn't
	 * currently supported on big-endian systems.
	 */
#else
	file_handle->fh_map = __wt_win_map;
	file_handle->fh_unmap = __wt_win_unmap;
#endif
	file_handle->fh_read = __win_file_read;
	file_handle->fh_size = __win_file_size;
	file_handle->fh_sync = __win_file_sync;
	file_handle->fh_truncate = __win_file_truncate;
	file_handle->fh_write = __win_file_write;

	*file_handlep = file_handle;

	return (0);

err:	WT_TRET(__win_file_close((WT_FILE_HANDLE *)win_fh, wt_session));
	return (ret);
}

/*
 * __win_terminate --
 *	Discard a Windows configuration.
 */
static int
__win_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	__wt_free(session, file_system);
	return (0);
}

/*
 * __wt_os_win --
 *	Initialize a MSVC configuration.
 */
int
__wt_os_win(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_FILE_SYSTEM *file_system;

	conn = S2C(session);

	WT_RET(__wt_calloc_one(session, &file_system));

	/* Initialize the Windows jump table. */
	file_system->fs_directory_list = __wt_win_directory_list;
	file_system->fs_directory_list_free = __wt_win_directory_list_free;
	file_system->fs_exist = __win_fs_exist;
	file_system->fs_open_file = __win_open_file;
	file_system->fs_remove = __win_fs_remove;
	file_system->fs_rename = __win_fs_rename;
	file_system->fs_size = __wt_win_fs_size;
	file_system->terminate = __win_terminate;

	/* Switch it into place. */
	conn->file_system = file_system;

	return (0);
}
