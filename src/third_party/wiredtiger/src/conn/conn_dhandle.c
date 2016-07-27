/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_dhandle_destroy --
 *	Destroy a data handle.
 */
static int
__conn_dhandle_destroy(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle)
{
	WT_DECL_RET;

	ret = __wt_rwlock_destroy(session, &dhandle->rwlock);
	__wt_free(session, dhandle->name);
	__wt_free(session, dhandle->checkpoint);
	__wt_free(session, dhandle->handle);
	__wt_spin_destroy(session, &dhandle->close_lock);
	__wt_overwrite_and_free(session, dhandle);

	return (ret);
}

/*
 * __conn_dhandle_alloc --
 *	Allocate a new data handle and return it linked into the connection's
 *	list.
 */
static int
__conn_dhandle_alloc(WT_SESSION_IMPL *session,
    const char *uri, const char *checkpoint, WT_DATA_HANDLE **dhandlep)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint64_t bucket;

	*dhandlep = NULL;

	WT_RET(__wt_calloc_one(session, &dhandle));

	WT_ERR(__wt_rwlock_alloc(session, &dhandle->rwlock, "data handle"));
	dhandle->name_hash = __wt_hash_city64(uri, strlen(uri));
	WT_ERR(__wt_strdup(session, uri, &dhandle->name));
	WT_ERR(__wt_strdup(session, checkpoint, &dhandle->checkpoint));

	/* TODO: abstract this out for other data handle types */
	WT_ERR(__wt_calloc_one(session, &btree));
	dhandle->handle = btree;
	btree->dhandle = dhandle;

	WT_ERR(__wt_spin_init(
	    session, &dhandle->close_lock, "data handle close"));

	__wt_stat_dsrc_init(dhandle);

	if (strcmp(uri, WT_METAFILE_URI) == 0)
		F_SET(dhandle, WT_DHANDLE_IS_METADATA);

	/*
	 * Prepend the handle to the connection list, assuming we're likely to
	 * need new files again soon, until they are cached by all sessions.
	 */
	bucket = dhandle->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_DHANDLE_INSERT(S2C(session), dhandle, bucket);

	*dhandlep = dhandle;
	return (0);

err:	WT_TRET(__conn_dhandle_destroy(session, dhandle));
	return (ret);
}

/*
 * __wt_conn_dhandle_find --
 *	Find a previously opened data handle.
 */
int
__wt_conn_dhandle_find(
    WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	conn = S2C(session);

	/* We must be holding the handle list lock at a higher level. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));

	bucket = __wt_hash_city64(uri, strlen(uri)) % WT_HASH_ARRAY_SIZE;
	if (checkpoint == NULL) {
		TAILQ_FOREACH(dhandle, &conn->dhhash[bucket], hashq) {
			if (F_ISSET(dhandle, WT_DHANDLE_DEAD))
				continue;
			if (dhandle->checkpoint == NULL &&
			    strcmp(uri, dhandle->name) == 0) {
				session->dhandle = dhandle;
				return (0);
			}
		}
	} else
		TAILQ_FOREACH(dhandle, &conn->dhhash[bucket], hashq) {
			if (F_ISSET(dhandle, WT_DHANDLE_DEAD))
				continue;
			if (dhandle->checkpoint != NULL &&
			    strcmp(uri, dhandle->name) == 0 &&
			    strcmp(checkpoint, dhandle->checkpoint) == 0) {
				session->dhandle = dhandle;
				return (0);
			}
		}

	WT_RET(__conn_dhandle_alloc(session, uri, checkpoint, &dhandle));

	session->dhandle = dhandle;
	return (0);
}

/*
 * __wt_conn_btree_sync_and_close --
 *	Sync and close the underlying btree handle.
 */
int
__wt_conn_btree_sync_and_close(WT_SESSION_IMPL *session, bool final, bool force)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	bool marked_dead, no_schema_lock;

	btree = S2BT(session);
	bm = btree->bm;
	dhandle = session->dhandle;
	marked_dead = false;

	if (!F_ISSET(dhandle, WT_DHANDLE_OPEN))
		return (0);

	/* Turn off eviction. */
	WT_RET(__wt_evict_file_exclusive_on(session));

	/*
	 * If we don't already have the schema lock, make it an error to try
	 * to acquire it.  The problem is that we are holding an exclusive
	 * lock on the handle, and if we attempt to acquire the schema lock
	 * we might deadlock with a thread that has the schema lock and wants
	 * a handle lock (specifically, checkpoint).
	 */
	no_schema_lock = false;
	if (!F_ISSET(session, WT_SESSION_LOCKED_SCHEMA)) {
		no_schema_lock = true;
		F_SET(session, WT_SESSION_NO_SCHEMA_LOCK);
	}

	/*
	 * We may not be holding the schema lock, and threads may be walking
	 * the list of open handles (for example, checkpoint).  Acquire the
	 * handle's close lock. We don't have the sweep server acquire the
	 * handle's rwlock so we have to prevent races through the close code.
	 */
	__wt_spin_lock(session, &dhandle->close_lock);

	/*
	 * The close can fail if an update cannot be written, return the EBUSY
	 * error to our caller for eventual retry.
	 *
	 * If we are forcing the close, just mark the handle dead and the tree
	 * will be discarded later.  Don't do this for memory-mapped trees: we
	 * have to close the file handle to allow the file to be removed, but
	 * memory mapped trees contain pointers into memory that will become
	 * invalid if the mapping is closed.
	 */
	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
		if (force && (bm == NULL || !bm->is_mapped(bm, session))) {
			F_SET(session->dhandle, WT_DHANDLE_DEAD);
			marked_dead = true;

			/* Reset the tree's eviction priority (if any). */
			__wt_evict_priority_clear(session);
		}
		if (!marked_dead || final)
			WT_ERR(__wt_checkpoint_close(session, final));
	}

	WT_TRET(__wt_btree_close(session));

	/*
	 * If we marked a handle dead it will be closed by sweep, via
	 * another call to sync and close.
	 */
	if (!marked_dead) {
		F_CLR(dhandle, WT_DHANDLE_OPEN);
		if (dhandle->checkpoint == NULL)
			--S2C(session)->open_btree_count;
	}
	WT_ASSERT(session,
	    F_ISSET(dhandle, WT_DHANDLE_DEAD) ||
	    !F_ISSET(dhandle, WT_DHANDLE_OPEN));

err:	__wt_spin_unlock(session, &dhandle->close_lock);

	if (no_schema_lock)
		F_CLR(session, WT_SESSION_NO_SCHEMA_LOCK);

	__wt_evict_file_exclusive_off(session);

	return (ret);
}

/*
 * __conn_btree_config_clear --
 *	Clear the underlying object's configuration information.
 */
static void
__conn_btree_config_clear(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;
	const char **a;

	dhandle = session->dhandle;

	if (dhandle->cfg == NULL)
		return;
	for (a = dhandle->cfg; *a != NULL; ++a)
		__wt_free(session, *a);
	__wt_free(session, dhandle->cfg);
}

/*
 * __conn_btree_config_set --
 *	Set up a btree handle's configuration information.
 */
static int
__conn_btree_config_set(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	char *metaconf;

	dhandle = session->dhandle;

	/*
	 * Read the object's entry from the metadata file, we're done if we
	 * don't find one.
	 */
	if ((ret =
	    __wt_metadata_search(session, dhandle->name, &metaconf)) != 0) {
		if (ret == WT_NOTFOUND)
			ret = ENOENT;
		WT_RET(ret);
	}

	/*
	 * The defaults are included because underlying objects have persistent
	 * configuration information stored in the metadata file.  If defaults
	 * are included in the configuration, we can add new configuration
	 * strings without upgrading the metadata file or writing special code
	 * in case a configuration string isn't initialized, as long as the new
	 * configuration string has an appropriate default value.
	 *
	 * The error handling is a little odd, but be careful: we're holding a
	 * chunk of allocated memory in metaconf.  If we fail before we copy a
	 * reference to it into the object's configuration array, we must free
	 * it, after the copy, we don't want to free it.
	 */
	WT_ERR(__wt_calloc_def(session, 3, &dhandle->cfg));
	WT_ERR(__wt_strdup(
	    session, WT_CONFIG_BASE(session, file_meta), &dhandle->cfg[0]));
	dhandle->cfg[1] = metaconf;
	return (0);

err:	__wt_free(session, metaconf);
	return (ret);
}

/*
 * __wt_conn_btree_open --
 *	Open the current btree handle.
 */
int
__wt_conn_btree_open(
    WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = session->dhandle;
	btree = S2BT(session);

	WT_ASSERT(session,
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
	    !LF_ISSET(WT_DHANDLE_LOCK_ONLY));

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_CLOSING));

	/*
	 * If the handle is already open, it has to be closed so it can
	 * be reopened with a new configuration.
	 *
	 * This call can return EBUSY if there's an update in the
	 * object that's not yet globally visible.  That's not a
	 * problem because it can only happen when we're switching from
	 * a normal handle to a "special" one, so we're returning EBUSY
	 * to an attempt to verify or do other special operations.  The
	 * reverse won't happen because when the handle from a verify
	 * or other special operation is closed, there won't be updates
	 * in the tree that can block the close.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session, false, false));

	/* Discard any previous configuration, set up the new configuration. */
	__conn_btree_config_clear(session);
	WT_RET(__conn_btree_config_set(session));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_MASK(WT_BTREE_SPECIAL_FLAGS));

	WT_ERR(__wt_btree_open(session, cfg));

	/*
	 * Bulk handles require true exclusive access, otherwise, handles
	 * marked as exclusive are allowed to be relocked by the same
	 * session.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
	    !LF_ISSET(WT_BTREE_BULK)) {
		dhandle->excl_session = session;
		dhandle->excl_ref = 1;
	}
	F_SET(dhandle, WT_DHANDLE_OPEN);

	/*
	 * Checkpoint handles are read only, so eviction calculations
	 * based on the number of btrees are better to ignore them.
	 */
	if (dhandle->checkpoint == NULL)
		++S2C(session)->open_btree_count;

	if (0) {
err:		F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
	}

	return (ret);
}

/*
 * __conn_btree_apply_internal --
 *	Apply a function to an open data handle.
 */
static int
__conn_btree_apply_internal(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
    int (*file_func)(WT_SESSION_IMPL *, const char *[]),
    int (*name_func)(WT_SESSION_IMPL *, const char *, bool *),
    const char *cfg[])
{
	WT_DECL_RET;
	bool skip;

	/* Always apply the name function, if supplied. */
	skip = false;
	if (name_func != NULL)
		WT_RET(name_func(session, dhandle->name, &skip));

	/* If there is no file function, don't bother locking the handle */
	if (file_func == NULL || skip)
		return (0);

	/*
	 * We need to pull the handle into the session handle cache and make
	 * sure it's referenced to stop other internal code dropping the handle
	 * (e.g in LSM when cleaning up obsolete chunks).
	 */
	if ((ret = __wt_session_get_btree(session,
	    dhandle->name, dhandle->checkpoint, NULL, 0)) != 0)
		return (ret == EBUSY ? 0 : ret);

	WT_SAVE_DHANDLE(session, ret = file_func(session, cfg));
	if (WT_META_TRACKING(session))
		WT_TRET(__wt_meta_track_handle_lock(session, false));
	else
		WT_TRET(__wt_session_release_btree(session));
	return (ret);
}

/*
 * __wt_conn_btree_apply --
 *	Apply a function to all open btree handles with the given URI.
 */
int
__wt_conn_btree_apply(WT_SESSION_IMPL *session, const char *uri,
    int (*file_func)(WT_SESSION_IMPL *, const char *[]),
    int (*name_func)(WT_SESSION_IMPL *, const char *, bool *),
    const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));

	/*
	 * If we're given a URI, then we walk only the hash list for that
	 * name.  If we don't have a URI we walk the entire dhandle list.
	 */
	if (uri != NULL) {
		bucket =
		    __wt_hash_city64(uri, strlen(uri)) % WT_HASH_ARRAY_SIZE;
		TAILQ_FOREACH(dhandle, &conn->dhhash[bucket], hashq) {
			if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
			    F_ISSET(dhandle, WT_DHANDLE_DEAD) ||
			    dhandle->checkpoint != NULL ||
			    strcmp(uri, dhandle->name) != 0)
				continue;
			WT_RET(__conn_btree_apply_internal(
			    session, dhandle, file_func, name_func, cfg));
		}
	} else {
		TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
			if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
			    F_ISSET(dhandle, WT_DHANDLE_DEAD) ||
			    dhandle->checkpoint != NULL ||
			    !WT_PREFIX_MATCH(dhandle->name, "file:") ||
			    WT_IS_METADATA(session, dhandle))
				continue;
			WT_RET(__conn_btree_apply_internal(
			    session, dhandle, file_func, name_func, cfg));
		}
	}

	return (0);
}

/*
 * __wt_conn_dhandle_close_all --
 *	Close all data handles handles with matching name (including all
 *	checkpoint handles).
 */
int
__wt_conn_dhandle_close_all(
    WT_SESSION_IMPL *session, const char *uri, bool force)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint64_t bucket;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));
	WT_ASSERT(session, session->dhandle == NULL);

	bucket = __wt_hash_city64(uri, strlen(uri)) % WT_HASH_ARRAY_SIZE;
	TAILQ_FOREACH(dhandle, &conn->dhhash[bucket], hashq) {
		if (strcmp(dhandle->name, uri) != 0 ||
		    F_ISSET(dhandle, WT_DHANDLE_DEAD))
			continue;

		session->dhandle = dhandle;

		/* Lock the handle exclusively. */
		WT_ERR(__wt_session_get_btree(session,
		    dhandle->name, dhandle->checkpoint,
		    NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));
		if (WT_META_TRACKING(session))
			WT_ERR(__wt_meta_track_handle_lock(session, false));

		/*
		 * We have an exclusive lock, which means there are no cursors
		 * open at this point.  Close the handle, if necessary.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			if ((ret = __wt_meta_track_sub_on(session)) == 0)
				ret = __wt_conn_btree_sync_and_close(
				    session, false, force);

			/*
			 * If the close succeeded, drop any locks it acquired.
			 * If there was a failure, this function will fail and
			 * the whole transaction will be rolled back.
			 */
			if (ret == 0)
				ret = __wt_meta_track_sub_off(session);
		}

		if (!WT_META_TRACKING(session))
			WT_TRET(__wt_session_release_btree(session));

		WT_ERR(ret);
	}

err:	session->dhandle = NULL;
	return (ret);
}

/*
 * __conn_dhandle_remove --
 *	Remove a handle from the shared list.
 */
static int
__conn_dhandle_remove(WT_SESSION_IMPL *session, bool final)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	conn = S2C(session);
	dhandle = session->dhandle;
	bucket = dhandle->name_hash % WT_HASH_ARRAY_SIZE;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));
	WT_ASSERT(session, dhandle != conn->cache->evict_file_next);

	/* Check if the handle was reacquired by a session while we waited. */
	if (!final &&
	    (dhandle->session_inuse != 0 || dhandle->session_ref != 0))
		return (EBUSY);

	WT_CONN_DHANDLE_REMOVE(conn, dhandle, bucket);
	return (0);

}

/*
 * __wt_conn_dhandle_discard_single --
 *	Close/discard a single data handle.
 */
int
__wt_conn_dhandle_discard_single(
    WT_SESSION_IMPL *session, bool final, bool force)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int tret;
	bool set_pass_intr;

	dhandle = session->dhandle;

	if (F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
	    (final && F_ISSET(dhandle, WT_DHANDLE_DEAD))) {
		tret = __wt_conn_btree_sync_and_close(session, final, force);
		if (final && tret != 0) {
			__wt_err(session, tret,
			    "Final close of %s failed", dhandle->name);
			WT_TRET(tret);
		} else if (!final)
			WT_RET(tret);
	}

	/*
	 * Kludge: interrupt the eviction server in case it is holding the
	 * handle list lock.
	 */
	set_pass_intr = false;
	if (!F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST)) {
		set_pass_intr = true;
		(void)__wt_atomic_add32(&S2C(session)->cache->pass_intr, 1);
	}

	/* Try to remove the handle, protected by the data handle lock. */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    tret = __conn_dhandle_remove(session, final));
	if (set_pass_intr)
		(void)__wt_atomic_sub32(&S2C(session)->cache->pass_intr, 1);
	WT_TRET(tret);

	/*
	 * After successfully removing the handle, clean it up.
	 */
	if (ret == 0 || final) {
		__conn_btree_config_clear(session);
		WT_TRET(__conn_dhandle_destroy(session, dhandle));
		session->dhandle = NULL;
	}

	return (ret);
}

/*
 * __wt_conn_dhandle_discard --
 *	Close/discard all data handles.
 */
int
__wt_conn_dhandle_discard(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * Empty the session cache: any data handles created in a connection
	 * method may be cached here, and we're about to close them.
	 */
	__wt_session_close_cache(session);

	/*
	 * Close open data handles: first, everything but the metadata file (as
	 * closing a normal file may open and write the metadata file), then
	 * the metadata file.
	 */
restart:
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (WT_IS_METADATA(session, dhandle))
			continue;

		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_conn_dhandle_discard_single(
		    session, true, F_ISSET(conn, WT_CONN_IN_MEMORY))));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our default
	 * session's list of open data handles, specifically, we added the
	 * metadata file if any of the files were dirty.  Clean up that list
	 * before we shut down the metadata entry, for good.
	 */
	__wt_session_close_cache(session);
	F_SET(session, WT_SESSION_NO_DATA_HANDLES);

	/*
	 * The connection may have an open metadata cursor handle. We cannot
	 * close it before now because it's potentially used when discarding
	 * other open data handles. Close it before discarding the underlying
	 * metadata handle.
	 */
	if (session->meta_cursor != NULL)
		WT_TRET(session->meta_cursor->close(session->meta_cursor));

	/* Close the metadata file handle. */
	while ((dhandle = TAILQ_FIRST(&conn->dhqh)) != NULL)
		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_conn_dhandle_discard_single(
		    session, true, F_ISSET(conn, WT_CONN_IN_MEMORY))));

	return (ret);
}
