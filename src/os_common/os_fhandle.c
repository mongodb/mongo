/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __fhandle_advise_notsup --
 *	POSIX fadvise unsupported.
 */
static int
__fhandle_advise_notsup(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, wt_off_t len, int advice)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(advice);

	/* Quietly fail, callers expect not-supported failures. */
	return (ENOTSUP);
}

/*
 * __fhandle_allocate_notsup --
 *	POSIX fallocate unsupported.
 */
static int
__fhandle_allocate_notsup(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: file-allocate", fh->name);
}

/*
 * __fhandle_close_notsup --
 *	ANSI C close/fclose unsupported.
 */
static int
__fhandle_close_notsup(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_RET_MSG(session, ENOTSUP, "%s: file-close", fh->name);
}

/*
 * __fhandle_lock_notsup --
 *	Lock/unlock a file unsupported.
 */
static int
__fhandle_lock_notsup(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	WT_UNUSED(lock);
	WT_RET_MSG(session, ENOTSUP, "%s: file-lock", fh->name);
}

/*
 * __fhandle_map_notsup --
 *	Map a file unsupported.
 */
static int
__fhandle_map_notsup(WT_SESSION_IMPL *session,
    WT_FH *fh, void *p, size_t *lenp, void **mappingcookie)
{
	WT_UNUSED(p);
	WT_UNUSED(lenp);
	WT_UNUSED(mappingcookie);
	WT_RET_MSG(session, ENOTSUP, "%s: file-map", fh->name);
}

/*
 * __fhandle_map_discard_notsup --
 *	Discard a section of a mapped region unsupported.
 */
static int
__fhandle_map_discard_notsup(
    WT_SESSION_IMPL *session, WT_FH *fh, void *p, size_t len)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: file-map-discard", fh->name);
}

/*
 * __fhandle_map_preload_notsup --
 *	Preload a section of a mapped region unsupported.
 */
static int
__fhandle_map_preload_notsup(
    WT_SESSION_IMPL *session, WT_FH *fh, const void *p, size_t len)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: file-map-preload", fh->name);
}

/*
 * __fhandle_map_unmap_notsup --
 *	Unmap a file unsupported.
 */
static int
__fhandle_map_unmap_notsup(WT_SESSION_IMPL *session,
    WT_FH *fh, void *p, size_t len, void **mappingcookie)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_UNUSED(mappingcookie);
	WT_RET_MSG(session, ENOTSUP, "%s: file-map-unmap", fh->name);
}

/*
 * __fhandle_read_notsup --
 *	POSIX pread unsupported.
 */
static int
__fhandle_read_notsup(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(buf);
	WT_RET_MSG(session, ENOTSUP, "%s: file-read", fh->name);
}

/*
 * __fhandle_size_notsup --
 *	Get the size of a file in bytes unsupported.
 */
static int
__fhandle_size_notsup(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_UNUSED(sizep);
	WT_RET_MSG(session, ENOTSUP, "%s: file-size", fh->name);
}

/*
 * __fhandle_sync_notsup --
 *	POSIX fsync unsupported.
 */
static int
__fhandle_sync_notsup(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_UNUSED(block);
	WT_RET_MSG(session, ENOTSUP, "%s: file-sync", fh->name);
}

/*
 * __fhandle_truncate_notsup --
 *	POSIX ftruncate.
 */
static int
__fhandle_truncate_notsup(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: file-truncate", fh->name);
}

/*
 * __fhandle_write_notsup --
 *	POSIX pwrite.
 */
static int
__fhandle_write_notsup(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(buf);
	WT_RET_MSG(session, ENOTSUP, "%s: file-write", fh->name);
}

/*
 * __fhandle_method_init --
 *	Initialize the WT_FH structure's methods to not-supported.
 */
static void
__fhandle_method_init(WT_FH *fh)
{
	/*
	 * Set up the initial set of handle methods to standard "not-supported"
	 * functions, the underlying open functions turn on supported functions.
	 */
	fh->fh_advise = __fhandle_advise_notsup;
	fh->fh_allocate = __fhandle_allocate_notsup;
	fh->fh_close = __fhandle_close_notsup;
	fh->fh_lock = __fhandle_lock_notsup;
	fh->fh_map = __fhandle_map_notsup;
	fh->fh_map_discard = __fhandle_map_discard_notsup;
	fh->fh_map_preload = __fhandle_map_preload_notsup;
	fh->fh_map_unmap = __fhandle_map_unmap_notsup;
	fh->fh_read = __fhandle_read_notsup;
	fh->fh_size = __fhandle_size_notsup;
	fh->fh_sync = __fhandle_sync_notsup;
	fh->fh_truncate = __fhandle_truncate_notsup;
	fh->fh_write = __fhandle_write_notsup;
}

/*
 * __wt_handle_search --
 *	Search for a matching handle.
 */
bool
__wt_handle_search(WT_SESSION_IMPL *session,
    const char *name, bool increment_ref, WT_FH *newfh, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;
	uint64_t bucket, hash;
	bool found;

	if (fhp != NULL)
		*fhp = NULL;

	conn = S2C(session);
	found = false;

	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;

	__wt_spin_lock(session, &conn->fh_lock);

	/*
	 * If we already have the file open, optionally increment the reference
	 * count and return a pointer.
	 */
	TAILQ_FOREACH(fh, &conn->fhhash[bucket], hashq)
		if (strcmp(name, fh->name) == 0) {
			if (increment_ref)
				++fh->ref;
			if (fhp != NULL)
				*fhp = fh;
			found = true;
			break;
		}

	/* If we don't find a match, optionally add a new entry. */
	if (!found && newfh != NULL) {
		newfh->name_hash = hash;
		WT_CONN_FILE_INSERT(conn, newfh, bucket);
		(void)__wt_atomic_add32(&conn->open_file_count, 1);

		if (increment_ref)
			++newfh->ref;
		if (fhp != NULL)
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
__open_verbose(WT_SESSION_IMPL *session,
    const char *name, uint32_t file_type, uint32_t flags)
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
	case WT_FILE_TYPE_CHECKPOINT:
		file_type_tag = "checkpoint";
		break;
	case WT_FILE_TYPE_DATA:
		file_type_tag = "data";
		break;
	case WT_FILE_TYPE_DIRECTORY:
		file_type_tag = "directory";
		break;
	case WT_FILE_TYPE_LOG:
		file_type_tag = "log";
		break;
	case WT_FILE_TYPE_REGULAR:
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
    const char *name, uint32_t file_type, uint32_t flags, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	bool lock_file, open_called;
	char *path;

	WT_ASSERT(session, file_type != 0);	/* A file type is required. */

	conn = S2C(session);
	fh = NULL;
	open_called = false;
	path = NULL;

	WT_RET(__open_verbose(session, name, file_type, flags));

	/* Check if the handle is already open. */
	if (__wt_handle_search(session, name, true, NULL, &fh)) {
		*fhp = fh;
		return (0);
	}

	/* Allocate and initialize the handle. */
	WT_ERR(__wt_calloc_one(session, &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));
	__fhandle_method_init(fh);

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

	/*
	 * Direct I/O: file-type is a flag from the set of possible flags stored
	 * in the connection handle during configuration, check for a match.
	 */
	fh->direct_io = false;
	if (FLD_ISSET(conn->direct_io, file_type))
		LF_SET(WT_OPEN_DIRECTIO);

	/* Create the path to the file. */
	if (!LF_ISSET(WT_OPEN_FIXED))
		WT_ERR(__wt_filename(session, name, &path));

	/* Call the underlying open function. */
	WT_ERR(conn->file_open(
	    session, fh, path == NULL ? name : path, file_type, flags));
	open_called = true;

	/*
	 * Repeat the check for a match: if there's no match, link our newly
	 * created handle onto the database's list of files.
	 */
	if (__wt_handle_search(session, name, true, fh, fhp)) {
err:		if (open_called)
			WT_TRET(fh->fh_close(session, fh));
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
	if ((fh->ref > 0 && --fh->ref > 0) || F_ISSET(fh, WT_FH_IN_MEMORY)) {
		__wt_spin_unlock(session, &conn->fh_lock);
		return (0);
	}

	/* Remove from the list. */
	bucket = fh->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_FILE_REMOVE(conn, fh, bucket);
	(void)__wt_atomic_sub32(&conn->open_file_count, 1);

	__wt_spin_unlock(session, &conn->fh_lock);

	/* Discard underlying resources. */
	ret = fh->fh_close(session, fh);

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
		/*
		 * In-memory configurations will have open files, but the ref
		 * counts should be zero.
		 */
		if (!F_ISSET(conn, WT_CONN_IN_MEMORY) || fh->ref != 0) {
			ret = EBUSY;
			__wt_errx(session,
			    "Connection has open file handles: %s", fh->name);
		}

		fh->ref = 1;
		F_CLR(fh, WT_FH_IN_MEMORY);

		WT_TRET(__wt_close(session, &fh));
	}
	return (ret);
}
