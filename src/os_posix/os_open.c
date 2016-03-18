/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_handle_search --
 *	Search for a matching handle.
 */
bool
__wt_handle_search(WT_SESSION_IMPL *session, const char *name,
    bool increment_ref, bool unlock, WT_FH *newfh, WT_FH **fhp)
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

	/*
	 * Our caller may be operating on the handle itself, optionally leave
	 * the list locked.
	 */
	if (unlock)
		__wt_spin_unlock(session, &conn->fh_lock);

	return (found);
}

/*
 * __wt_handle_search_unlock --
 *	Release handle lock.
 */
void
__wt_handle_search_unlock(WT_SESSION_IMPL *session)
{
	__wt_spin_unlock(session, &S2C(session)->fh_lock);
}

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session,
    const char *name, int dio_type, u_int flags, WT_FH **fhp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	bool open_called;

	conn = S2C(session);

	fh = NULL;
	open_called = false;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: open", name));

	/* Check if the handle is already open. */
	if (__wt_handle_search(session, name, true, true, NULL, &fh)) {
		/*
		 * XXX
		 * The in-memory implementation has to reset the file offset
		 * when a file is re-opened (which obviously also depends on
		 * in-memory configurations never opening a file in more than
		 * one thread at a time). This needs to be fixed.
		 */
		if (F_ISSET(fh, WT_FH_IN_MEMORY) && fh->ref == 1)
			fh->off = 0;
		*fhp = fh;
		return (0);
	}

	/* Allocate a structure and set the name. */
	WT_ERR(__wt_calloc_one(session, &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));

	/* Configure fallocate/posix_fallocate calls. */
	__wt_fallocate_config(session, fh);

	/*
	 * If this is a read-only connection, open all files read-only except
	 * the lock file.
	 */
	if (F_ISSET(conn, WT_CONN_READONLY) &&
	    !WT_STRING_MATCH(name, WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)))
		LF_SET(WT_OPEN_READONLY);

	/*
	 * The only file created in read-only mode is the lock file.
	 */
	WT_ASSERT(session,
	    !LF_ISSET(WT_OPEN_CREATE) ||
	    !F_ISSET(conn, WT_CONN_READONLY) ||
	    WT_STRING_MATCH(name, WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)));

	/* Call the underlying open function. */
	WT_ERR(WT_JUMP(j_handle_open, session, fh, name, dio_type, flags));
	open_called = true;

	/* Set the file's size. */
	WT_ERR(WT_JUMP(j_handle_size, session, fh, &fh->size));

	/*
	 * Repeat the check for a match: if there's no match, link our newly
	 * created handle onto the database's list of files.
	 */
	if (__wt_handle_search(session, name, true, true, fh, fhp)) {
err:		if (open_called)
			WT_TRET(WT_JUMP(j_handle_close, session, fh));
		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
	}
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

	/* Catch attempts to close the standard streams. */
	if (fh == WT_STDERR || fh == WT_STDOUT)
		return (EINVAL);

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: close", fh->name));

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
	ret = WT_JUMP(j_handle_close, session, fh);

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
