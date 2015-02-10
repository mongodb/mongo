/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session,
    const char *name, int ok_create, int exclusive, int dio_type, WT_FH **fhp)
{
	DWORD dwCreationDisposition;
	HANDLE filehandle, filehandle_secondary;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh, *tfh;
	uint64_t bucket, hash;
	int direct_io, f, matched, share_mode;
	char *path;

	conn = S2C(session);
	fh = NULL;
	path = NULL;
	filehandle = INVALID_HANDLE_VALUE;
	filehandle_secondary = INVALID_HANDLE_VALUE;
	direct_io = 0;
	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: open", name));

	/* Increment the reference count if we already have the file open. */
	matched = 0;
	__wt_spin_lock(session, &conn->fh_lock);
	SLIST_FOREACH(tfh, &conn->fhhash[bucket], l)
		if (strcmp(name, tfh->name) == 0) {
			++tfh->ref;
			*fhp = tfh;
			matched = 1;
			break;
		}
	__wt_spin_unlock(session, &conn->fh_lock);
	if (matched)
		return (0);

	/* For directories, create empty file handles with invalid handles */
	if (dio_type == WT_FILE_TYPE_DIRECTORY) {
		goto setupfh;
	}

	WT_RET(__wt_filename(session, name, &path));

	share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
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
	if (ok_create) {
		dwCreationDisposition = CREATE_NEW;
		if (exclusive)
			dwCreationDisposition = CREATE_ALWAYS;
	} else
		dwCreationDisposition = OPEN_EXISTING;

	if (dio_type && FLD_ISSET(conn->direct_io, dio_type)) {
		f |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
		direct_io = 1;
	}

	if (dio_type == WT_FILE_TYPE_LOG &&
	    FLD_ISSET(conn->txn_logsync, WT_LOG_DSYNC)) {
		f |= FILE_FLAG_WRITE_THROUGH;
	}

	/* Disable read-ahead on trees: it slows down random read workloads. */
	if (dio_type == WT_FILE_TYPE_DATA ||
	    dio_type == WT_FILE_TYPE_CHECKPOINT)
		f |= FILE_FLAG_RANDOM_ACCESS;

	filehandle = CreateFileA(path,
				(GENERIC_READ | GENERIC_WRITE),
				share_mode,
				NULL,
				dwCreationDisposition,
				f,
				NULL);
	if (filehandle == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_FILE_EXISTS && ok_create)
			filehandle = CreateFileA(path,
						(GENERIC_READ | GENERIC_WRITE),
						share_mode,
						NULL,
						OPEN_EXISTING,
						f,
						NULL);

		if (filehandle == INVALID_HANDLE_VALUE)
			WT_ERR_MSG(session, __wt_errno(),
			    direct_io ?
			    "%s: open failed with direct I/O configured, some "
			    "filesystem types do not support direct I/O" :
			    "%s", path);
	}

	/*
	 * Open a second handle to file to support allocation/truncation
	 * concurrently with reads on the file. Writes would also move the file
	 * pointer.
	 */
	filehandle_secondary = CreateFileA(path,
	    (GENERIC_READ | GENERIC_WRITE),
	    share_mode,
	    NULL,
	    OPEN_EXISTING,
	    f,
	    NULL);
	if (filehandle == INVALID_HANDLE_VALUE)
		WT_ERR_MSG(session, __wt_errno(),
		    "open failed for secondary handle: %s", path);

setupfh:
	WT_ERR(__wt_calloc_one(session, &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));
	fh->name_hash = hash;
	fh->filehandle = filehandle;
	fh->filehandle_secondary = filehandle_secondary;
	fh->ref = 1;
	fh->direct_io = direct_io;

	/* Set the file's size. */
	if (dio_type != WT_FILE_TYPE_DIRECTORY)
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
	SLIST_FOREACH(tfh, &conn->fhhash[bucket], l)
		if (strcmp(name, tfh->name) == 0) {
			++tfh->ref;
			*fhp = tfh;
			matched = 1;
			break;
		}
	if (!matched) {
		WT_CONN_FILE_INSERT(conn, fh, bucket);
		WT_STAT_FAST_CONN_INCR(session, file_open);

		*fhp = fh;
	}
	__wt_spin_unlock(session, &conn->fh_lock);
	if (matched) {
err:		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
		if (filehandle != INVALID_HANDLE_VALUE)
			(void)CloseHandle(filehandle);
		if (filehandle_secondary != INVALID_HANDLE_VALUE)
			(void)CloseHandle(filehandle_secondary);
	}

	__wt_free(session, path);
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
	uint64_t bucket;

	conn = S2C(session);

	__wt_spin_lock(session, &conn->fh_lock);
	if (fh == NULL || fh->ref == 0 || --fh->ref > 0) {
		__wt_spin_unlock(session, &conn->fh_lock);
		return (0);
	}

	/* Remove from the list. */
	bucket = fh->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_FILE_REMOVE(conn, fh, bucket);
	WT_STAT_FAST_CONN_DECR(session, file_open);

	__wt_spin_unlock(session, &conn->fh_lock);

	/* Discard the memory.
	 * Note: For directories, we do not open valid directory handles on
	 * windows since it is not possible to sync a directory
	 */
	if (fh->filehandle != INVALID_HANDLE_VALUE &&
	    CloseHandle(fh->filehandle) == 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "CloseHandle: %s", fh->name);
	}

	if (fh->filehandle_secondary != INVALID_HANDLE_VALUE &&
	    CloseHandle(fh->filehandle_secondary) == 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "CloseHandle: secondary: %s", fh->name);
	}

	__wt_free(session, fh->name);
	__wt_free(session, fh);
	return (ret);
}
