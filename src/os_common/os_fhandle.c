/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __fhandle_method_finalize --
 *	Initialize any NULL WT_FH structure methods to not-supported. Doing
 *	this means that custom file systems with incomplete implementations
 *	won't dereference NULL pointers.
 */
static int
__fhandle_method_finalize(
    WT_SESSION_IMPL *session, WT_FILE_HANDLE *handle, bool readonly)
{
#define	WT_HANDLE_METHOD_REQ(name)					\
	if (handle->name == NULL)					\
		WT_RET_MSG(session, EINVAL,				\
		    "a WT_FILE_HANDLE.%s method must be configured", #name)

	WT_HANDLE_METHOD_REQ(close);
	/* not required: fadvise */
	/* not required: fallocate */
	/* not required: fallocate_nolock */
	WT_HANDLE_METHOD_REQ(fh_lock);
	/* not required: map */
	/* not required: map_discard */
	/* not required: map_preload */
	/* not required: map_unmap */
	WT_HANDLE_METHOD_REQ(fh_read);
	WT_HANDLE_METHOD_REQ(fh_size);
	if (!readonly)
		WT_HANDLE_METHOD_REQ(fh_sync);
	/* not required: sync_nowait */
	if (!readonly) {
		WT_HANDLE_METHOD_REQ(fh_truncate);
		WT_HANDLE_METHOD_REQ(fh_write);
	}

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_handle_is_open --
 *	Return if there's an open handle matching a name.
 */
bool
__wt_handle_is_open(WT_SESSION_IMPL *session, const char *name)
{
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;
	uint64_t bucket, hash;
	bool found;

	conn = S2C(session);
	found = false;

	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;

	__wt_spin_lock(session, &conn->fh_lock);

	TAILQ_FOREACH(fh, &conn->fhhash[bucket], hashq)
		if (strcmp(name, fh->name) == 0) {
			found = true;
			break;
		}

	__wt_spin_unlock(session, &conn->fh_lock);

	return (found);
}
#endif

/*
 * __handle_search --
 *	Search for a matching handle.
 */
static bool
__handle_search(
    WT_SESSION_IMPL *session, const char *name, WT_FH *newfh, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;
	uint64_t bucket, hash;
	bool found;

	*fhp = NULL;

	conn = S2C(session);
	found = false;

	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;

	__wt_spin_lock(session, &conn->fh_lock);

	/*
	 * If we already have the file open, increment the reference count and
	 * return a pointer.
	 */
	TAILQ_FOREACH(fh, &conn->fhhash[bucket], hashq)
		if (strcmp(name, fh->name) == 0) {
			++fh->ref;
			*fhp = fh;
			found = true;
			break;
		}

	/* If we don't find a match, optionally add a new entry. */
	if (!found && newfh != NULL) {
		newfh->name_hash = hash;
		WT_FILE_HANDLE_INSERT(conn, newfh, bucket);
		(void)__wt_atomic_add32(&conn->open_file_count, 1);

		++newfh->ref;
		*fhp = newfh;
	}

	__wt_spin_unlock(session, &conn->fh_lock);

	return (found);
}

/*
 * __open_verbose --
 *	Optionally output a verbose message on handle open.
 */
static inline int
__open_verbose(
    WT_SESSION_IMPL *session, const char *name, int file_type, u_int flags)
{
#ifdef HAVE_VERBOSE
	WT_DECL_RET;
	WT_DECL_ITEM(tmp);
	const char *file_type_tag, *sep;

	if (!WT_VERBOSE_ISSET(session, WT_VERB_FILEOPS))
		return (0);

	/*
	 * It's useful to track file opens when debugging platforms, take some
	 * effort to output good tracking information.
	 */

	switch (file_type) {
	case WT_OPEN_FILE_TYPE_CHECKPOINT:
		file_type_tag = "checkpoint";
		break;
	case WT_OPEN_FILE_TYPE_DATA:
		file_type_tag = "data";
		break;
	case WT_OPEN_FILE_TYPE_DIRECTORY:
		file_type_tag = "directory";
		break;
	case WT_OPEN_FILE_TYPE_LOG:
		file_type_tag = "log";
		break;
	case WT_OPEN_FILE_TYPE_REGULAR:
		file_type_tag = "regular";
		break;
	default:
		file_type_tag = "unknown open type";
		break;
	}

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	sep = " (";
#define	WT_OPEN_VERBOSE_FLAG(f, name)					\
	if (LF_ISSET(f)) {						\
		WT_ERR(__wt_buf_catfmt(					\
		    session, tmp, "%s%s", sep, name));			\
		sep = ", ";						\
	}

	WT_OPEN_VERBOSE_FLAG(WT_OPEN_CREATE, "create");
	WT_OPEN_VERBOSE_FLAG(WT_OPEN_DIRECTIO, "direct-IO");
	WT_OPEN_VERBOSE_FLAG(WT_OPEN_EXCLUSIVE, "exclusive");
	WT_OPEN_VERBOSE_FLAG(WT_OPEN_FIXED, "fixed");
	WT_OPEN_VERBOSE_FLAG(WT_OPEN_READONLY, "readonly");

	if (tmp->size != 0)
		WT_ERR(__wt_buf_catfmt(session, tmp, ")"));

	ret = __wt_verbose(session, WT_VERB_FILEOPS,
	    "%s: file-open: type %s%s",
	    name, file_type_tag, tmp->size == 0 ? "" : (char *)tmp->data);

err:	__wt_scr_free(session, &tmp);
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(file_type);
	WT_UNUSED(flags);
	return (0);
#endif
}

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session,
    const char *name, WT_OPEN_FILE_TYPE file_type, u_int flags, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	WT_FILE_SYSTEM *file_system;
	bool lock_file, open_called;
	char *path;

	WT_ASSERT(session, file_type != 0);	/* A file type is required. */

	conn = S2C(session);
	file_system = conn->file_system;
	fh = NULL;
	open_called = false;
	path = NULL;

	WT_RET(__open_verbose(session, name, file_type, flags));

	/* Check if the handle is already open. */
	if (__handle_search(session, name, NULL, &fh)) {
		*fhp = fh;
		return (0);
	}

	/* Allocate and initialize the handle. */
	WT_ERR(__wt_calloc_one(session, &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));

	/*
	 * If this is a read-only connection, open all files read-only except
	 * the lock file.
	 *
	 * The only file created in read-only mode is the lock file.
	 */
	if (F_ISSET(conn, WT_CONN_READONLY)) {
		lock_file = strcmp(name, WT_SINGLETHREAD) == 0;
		if (!lock_file)
			LF_SET(WT_OPEN_READONLY);
		WT_ASSERT(session, lock_file || !LF_ISSET(WT_OPEN_CREATE));
	}

	/* Create the path to the file. */
	if (!LF_ISSET(WT_OPEN_FIXED))
		WT_ERR(__wt_filename(session, name, &path));

	/* Call the underlying open function. */
	WT_ERR(file_system->fs_open_file(file_system, &session->iface,
	    path == NULL ? name : path, file_type, flags, &fh->handle));
	open_called = true;

	WT_ERR(__fhandle_method_finalize(
	    session, fh->handle, LF_ISSET(WT_OPEN_READONLY)));

	/*
	 * Repeat the check for a match: if there's no match, link our newly
	 * created handle onto the database's list of files.
	 */
	if (__handle_search(session, name, fh, fhp)) {
err:		if (open_called)
			WT_TRET(fh->handle->close(
			    fh->handle, (WT_SESSION *)session));
		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
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

	/* Track handle-close as a file operation, so open and close match. */
	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: file-close", fh->name));

	/*
	 * If the reference count hasn't gone to 0, or if it's an in-memory
	 * object, we're done.
	 *
	 * Assert the reference count is correct, but don't let it wrap.
	 */
	__wt_spin_lock(session, &conn->fh_lock);
	WT_ASSERT(session, fh->ref > 0);
	if ((fh->ref > 0 && --fh->ref > 0)) {
		__wt_spin_unlock(session, &conn->fh_lock);
		return (0);
	}

	/* Remove from the list. */
	bucket = fh->name_hash % WT_HASH_ARRAY_SIZE;
	WT_FILE_HANDLE_REMOVE(conn, fh, bucket);
	(void)__wt_atomic_sub32(&conn->open_file_count, 1);

	__wt_spin_unlock(session, &conn->fh_lock);

	/* Discard underlying resources. */
	ret = fh->handle->close(fh->handle, (WT_SESSION *)session);

	__wt_free(session, fh->name);
	__wt_free(session, fh);

	return (ret);
}

/*
 * __wt_close_connection_close --
 *	Close any open file handles at connection close.
 */
int
__wt_close_connection_close(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_FH *fh;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	while ((fh = TAILQ_FIRST(&conn->fhqh)) != NULL) {
		if (fh->ref != 0) {
			ret = EBUSY;
			__wt_errx(session,
			    "Connection has open file handles: %s", fh->name);
		}

		fh->ref = 1;

		WT_TRET(__wt_close(session, &fh));
	}
	return (ret);
}
