/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_dhandle_open_lock --
 *	Spin on the current data handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.  If exclusive access is
 *	requested and cannot be granted immediately because the handle is
 *	in use, fail with EBUSY.
 *
 *	Here is a brief summary of how different operations synchronize using
 *	either the schema lock, handle locks or handle flags:
 *
 *	open -- holds the schema lock, one thread gets the handle exclusive,
 *		reverts to a shared handle lock and drops the schema lock
 *		once the handle is open;
 *	bulk load -- sets bulk and exclusive;
 *	salvage, truncate, update, verify -- hold the schema lock, set a
 *		"special" flag;
 *	sweep -- gets a write lock on the handle, doesn't set exclusive
 *
 *	The schema lock prevents a lot of potential conflicts: we should never
 *	see handles being salvaged or verified because those operation hold the
 *	schema lock.  However, it is possible to see a handle that is being
 *	bulk loaded, or that the sweep server is closing.
 *
 *	The principle here is that application operations can cause other
 *	application operations to fail (so attempting to open a cursor on a
 *	file while it is being bulk-loaded will fail), but internal or
 *	database-wide operations should not prevent application-initiated
 *	operations.  For example, attempting to verify a file should not fail
 *	because the sweep server happens to be in the process of closing that
 *	file.
 */
static int
__conn_dhandle_open_lock(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	int is_open, lock_busy, want_exclusive;

	btree = dhandle->handle;
	lock_busy = 0;
	want_exclusive = LF_ISSET(WT_DHANDLE_EXCLUSIVE) ? 1 : 0;

	/*
	 * Check that the handle is open.  We've already incremented
	 * the reference count, so once the handle is open it won't be
	 * closed by another thread.
	 *
	 * If we can see the WT_DHANDLE_OPEN flag set while holding a
	 * lock on the handle, then it's really open and we can start
	 * using it.  Alternatively, if we can get an exclusive lock
	 * and WT_DHANDLE_OPEN is still not set, we need to do the open.
	 */
	for (;;) {
		/*
		 * If the handle is already open for a special operation,
		 * give up.
		 */
		if (F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))
			return (EBUSY);

		/*
		 * If the handle is open, get a read lock and recheck.
		 *
		 * Wait for a read lock if we want exclusive access and failed
		 * to get it: the sweep server may be closing this handle, and
		 * we need to wait for it to complete.  If we want exclusive
		 * access and find the handle open once we get the read lock,
		 * give up: some other thread has it locked for real.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    (!want_exclusive || lock_busy)) {
			WT_RET(__wt_readlock(session, dhandle->rwlock));
			is_open = F_ISSET(dhandle, WT_DHANDLE_OPEN) ? 1 : 0;
			if (is_open && !want_exclusive)
				return (0);
			WT_RET(__wt_readunlock(session, dhandle->rwlock));
		} else
			is_open = 0;

		/*
		 * It isn't open or we want it exclusive: try to get an
		 * exclusive lock.  There is some subtlety here: if we race
		 * with another thread that successfully opens the file, we
		 * don't want to block waiting to get exclusive access.
		 */
		if ((ret = __wt_try_writelock(session, dhandle->rwlock)) == 0) {
			/*
			 * If it was opened while we waited, drop the write
			 * lock and get a read lock instead.
			 */
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
			    !want_exclusive) {
				lock_busy = 0;
				WT_RET(
				    __wt_writeunlock(session, dhandle->rwlock));
				continue;
			}

			/* We have an exclusive lock, we're done. */
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
			return (0);
		} else if (ret != EBUSY || (is_open && want_exclusive))
			return (ret);
		else
			lock_busy = 1;

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}
}

/*
 * __wt_conn_dhandle_find --
 *	Find a previously opened data handle.
 */
int
__wt_conn_dhandle_find(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	WT_UNUSED(flags);	/* Only used in diagnostic builds */
	conn = S2C(session);

	/* We must be holding the handle list lock at a higher level. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED) &&
	    !LF_ISSET(WT_DHANDLE_HAVE_REF));

	/* Increment the reference count if we already have the btree open. */
	bucket = __wt_hash_city64(name, strlen(name)) % WT_HASH_ARRAY_SIZE;
	SLIST_FOREACH(dhandle, &conn->dhhash[bucket], hashl)
		if (strcmp(name, dhandle->name) == 0 &&
		    ((ckpt == NULL && dhandle->checkpoint == NULL) ||
		    (ckpt != NULL && dhandle->checkpoint != NULL &&
		    strcmp(ckpt, dhandle->checkpoint) == 0))) {
			session->dhandle = dhandle;
			return (0);
		}

	return (WT_NOTFOUND);
}

/*
 * __conn_dhandle_get --
 *	Allocate a new data handle, lock it exclusively, and return it linked
 *	into the connection's list.
 */
static int
__conn_dhandle_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint32_t bucket;

	conn = S2C(session);

	/*
	 * We have the handle lock, check whether we can find the handle we
	 * are looking for.  If we do, and we can lock it in the state we
	 * want, this session will take ownership and we are done.
	 */
	ret = __wt_conn_dhandle_find(session, name, ckpt, flags);
	if (ret == 0) {
		dhandle = session->dhandle;
		WT_RET(__conn_dhandle_open_lock(session, dhandle, flags));
		return (0);
	}
	WT_RET_NOTFOUND_OK(ret);

	/*
	 * If no handle was found, allocate the data handle and a btree handle,
	 * then initialize the data handle.  Exclusively lock the data handle
	 * before inserting it in the list.
	 */
	WT_RET(__wt_calloc_one(session, &dhandle));

	WT_ERR(__wt_rwlock_alloc(session, &dhandle->rwlock, "data handle"));

	dhandle->name_hash = __wt_hash_city64(name, strlen(name));
	WT_ERR(__wt_strdup(session, name, &dhandle->name));
	if (ckpt != NULL)
		WT_ERR(__wt_strdup(session, ckpt, &dhandle->checkpoint));

	WT_ERR(__wt_calloc_one(session, &btree));
	dhandle->handle = btree;
	btree->dhandle = dhandle;

	WT_ERR(__wt_spin_init(
	    session, &dhandle->close_lock, "data handle close"));

	F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
	WT_ERR(__wt_writelock(session, dhandle->rwlock));

	/*
	 * Prepend the handle to the connection list, assuming we're likely to
	 * need new files again soon, until they are cached by all sessions.
	 * Find the right hash bucket to insert into as well.
	 */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));
	bucket = dhandle->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_DHANDLE_INSERT(conn, dhandle, bucket);

	session->dhandle = dhandle;
	return (0);

err:	WT_TRET(__wt_rwlock_destroy(session, &dhandle->rwlock));
	__wt_free(session, dhandle->name);
	__wt_free(session, dhandle->checkpoint);
	__wt_free(session, dhandle->handle);		/* btree free */
	__wt_spin_destroy(session, &dhandle->close_lock);
	__wt_overwrite_and_free(session, dhandle);

	return (ret);
}

/*
 * __wt_conn_btree_sync_and_close --
 *	Sync and close the underlying btree handle.
 */
int
__wt_conn_btree_sync_and_close(WT_SESSION_IMPL *session, int force)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int no_schema_lock;

	dhandle = session->dhandle;
	btree = S2BT(session);

	if (!F_ISSET(dhandle, WT_DHANDLE_OPEN))
		return (0);

	/*
	 * If we don't already have the schema lock, make it an error to try
	 * to acquire it.  The problem is that we are holding an exclusive
	 * lock on the handle, and if we attempt to acquire the schema lock
	 * we might deadlock with a thread that has the schema lock and wants
	 * a handle lock (specifically, checkpoint).
	 */
	no_schema_lock = 0;
	if (!F_ISSET(session, WT_SESSION_SCHEMA_LOCKED)) {
		no_schema_lock = 1;
		F_SET(session, WT_SESSION_NO_SCHEMA_LOCK);
	}

	/*
	 * We may not be holding the schema lock, and threads may be walking
	 * the list of open handles (for example, checkpoint).  Acquire the
	 * handle's close lock.
	 */
	__wt_spin_lock(session, &dhandle->close_lock);

	/*
	 * The close can fail if an update cannot be written, return the EBUSY
	 * error to our caller for eventual retry.
	 */
	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		WT_ERR(__wt_checkpoint_close(session, force));

	if (dhandle->checkpoint == NULL)
		--S2C(session)->open_btree_count;

	WT_TRET(__wt_btree_close(session));
	F_CLR(dhandle, WT_DHANDLE_OPEN);
	F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);

err:	__wt_spin_unlock(session, &dhandle->close_lock);

	if (no_schema_lock)
		F_CLR(session, WT_SESSION_NO_SCHEMA_LOCK);

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
 * __conn_btree_open --
 *	Open the current btree handle.
 */
static int
__conn_btree_open(
	WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = session->dhandle;
	btree = S2BT(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) &&
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
	    !LF_ISSET(WT_DHANDLE_LOCK_ONLY));

	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_CLOSING));

	/*
	 * If the handle is already open, it has to be closed so it can be
	 * reopened with a new configuration.  We don't need to check again:
	 * this function isn't called if the handle is already open in the
	 * required mode.
	 *
	 * This call can return EBUSY if there's an update in the object that's
	 * not yet globally visible.  That's not a problem because it can only
	 * happen when we're switching from a normal handle to a "special" one,
	 * so we're returning EBUSY to an attempt to verify or do other special
	 * operations.  The reverse won't happen because when the handle from a
	 * verify or other special operation is closed, there won't be updates
	 * in the tree that can block the close.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session, 0));

	/* Discard any previous configuration, set up the new configuration. */
	__conn_btree_config_clear(session);
	WT_RET(__conn_btree_config_set(session));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

	do {
		WT_ERR(__wt_btree_open(session, cfg));
		F_SET(dhandle, WT_DHANDLE_OPEN);
		/*
		 * Checkpoint handles are read only, so eviction calculations
		 * based on the number of btrees are better to ignore them.
		 */
		if (dhandle->checkpoint == NULL)
			++S2C(session)->open_btree_count;

		/* Drop back to a readlock if that is all that was needed. */
		if (!LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			WT_ERR(__wt_writeunlock(session, dhandle->rwlock));
			WT_ERR(
			    __conn_dhandle_open_lock(session, dhandle, flags));
		}
	} while (!F_ISSET(dhandle, WT_DHANDLE_OPEN));

	if (0) {
err:		F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
		/* If the open failed, close the handle. */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
			WT_TRET(__wt_conn_btree_sync_and_close(session, 0));
	}

	return (ret);
}

/*
 * __wt_conn_btree_get --
 *	Get an open btree file handle, otherwise open a new one.
 */
int
__wt_conn_btree_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, const char *cfg[], uint32_t flags)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	if (LF_ISSET(WT_DHANDLE_HAVE_REF))
		WT_RET(
		    __conn_dhandle_open_lock(session, session->dhandle, flags));
	else {
		WT_WITH_DHANDLE_LOCK(session,
		    ret = __conn_dhandle_get(session, name, ckpt, flags));
		WT_RET(ret);
	}
	dhandle = session->dhandle;

	if (!LF_ISSET(WT_DHANDLE_LOCK_ONLY) &&
	    (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
	    LF_ISSET(WT_BTREE_SPECIAL_FLAGS)))
		if ((ret = __conn_btree_open(session, cfg, flags)) != 0) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			WT_TRET(__wt_writeunlock(session, dhandle->rwlock));
		}

	WT_ASSERT(session, ret != 0 ||
	    LF_ISSET(WT_DHANDLE_EXCLUSIVE) ==
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));

	return (ret);
}

/*
 * __conn_btree_apply_internal --
 *	Apply a function to the open btree handles.
 */
static int
__conn_btree_apply_internal(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_DECL_RET;

	/*
	 * We need to pull the handle into the session handle cache and make
	 * sure it's referenced to stop other internal code dropping the handle
	 * (e.g in LSM when cleaning up obsolete chunks).
	 */
	ret = __wt_session_get_btree(session,
	    dhandle->name, dhandle->checkpoint, NULL, 0);
	if (ret == 0) {
		WT_SAVE_DHANDLE(session,
		    ret = func(session, cfg));
		if (WT_META_TRACKING(session))
			WT_TRET(__wt_meta_track_handle_lock(session, 0));
		else
			WT_TRET(__wt_session_release_btree(session));
	} else if (ret == EBUSY)
		ret = __wt_conn_btree_apply_single(session, dhandle->name,
		    dhandle->checkpoint, func, cfg);
	return (ret);
}

/*
 * __wt_conn_btree_apply --
 *	Apply a function to all open btree handles apart from the metadata.
 */
int
__wt_conn_btree_apply(WT_SESSION_IMPL *session,
    int apply_checkpoints, const char *uri,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));

	/*
	 * If we're given a URI, then we walk only the hash list for that
	 * name.  If we don't have a URI we walk the entire dhandle list.
	 */
	if (uri != NULL) {
		bucket =
		    __wt_hash_city64(uri, strlen(uri)) % WT_HASH_ARRAY_SIZE;
		SLIST_FOREACH(dhandle, &conn->dhhash[bucket], hashl)
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
			    strcmp(uri, dhandle->name) == 0 &&
			    (apply_checkpoints || dhandle->checkpoint == NULL))
				WT_RET(__conn_btree_apply_internal(
				    session, dhandle, func, cfg));
	} else {
		SLIST_FOREACH(dhandle, &conn->dhlh, l)
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
			    (apply_checkpoints ||
			    dhandle->checkpoint == NULL) &&
			    WT_PREFIX_MATCH(dhandle->name, "file:") &&
			    !WT_IS_METADATA(dhandle))
				WT_RET(__conn_btree_apply_internal(
				    session, dhandle, func, cfg));
	}

	return (0);
}

/*
 * __wt_conn_btree_apply_single_ckpt --
 *	Decode any checkpoint information from the configuration string then
 *	call btree apply single.
 */
int
__wt_conn_btree_apply_single_ckpt(WT_SESSION_IMPL *session,
    const char *uri,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *checkpoint;

	checkpoint = NULL;

	/*
	 * This function exists to handle checkpoint configuration.  Callers
	 * that never open a checkpoint call the underlying function directly.
	 */
	WT_RET_NOTFOUND_OK(
	    __wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
	if (cval.len != 0) {
		/*
		 * The internal checkpoint name is special, find the last
		 * unnamed checkpoint of the object.
		 */
		if (WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
			WT_RET(__wt_meta_checkpoint_last_name(
			    session, uri, &checkpoint));
		} else
			WT_RET(__wt_strndup(
			    session, cval.str, cval.len, &checkpoint));
	}

	ret = __wt_conn_btree_apply_single(session, uri, checkpoint, func, cfg);

	__wt_free(session, checkpoint);

	return (ret);
}

/*
 * __wt_conn_btree_apply_single --
 *	Apply a function to a single btree handle that couldn't be locked
 * (attempting to get the handle returned EBUSY).
 */
int
__wt_conn_btree_apply_single(WT_SESSION_IMPL *session,
    const char *uri, const char *checkpoint,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint64_t bucket, hash;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));

	hash = __wt_hash_city64(uri, strlen(uri));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	SLIST_FOREACH(dhandle, &conn->dhhash[bucket], hashl)
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    (hash == dhandle->name_hash &&
		     strcmp(uri, dhandle->name) == 0) &&
		    ((dhandle->checkpoint == NULL && checkpoint == NULL) ||
		    (dhandle->checkpoint != NULL && checkpoint != NULL &&
		    strcmp(dhandle->checkpoint, checkpoint) == 0))) {
			/*
			 * We're holding the handle list lock which locks out
			 * handle open (which might change the state of the
			 * underlying object).  However, closing a handle
			 * doesn't require the handle list lock, lock out
			 * closing the handle and then confirm the handle is
			 * still open.
			 */
			__wt_spin_lock(session, &dhandle->close_lock);
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
				WT_WITH_DHANDLE(session, dhandle,
				    ret = func(session, cfg));
			}
			__wt_spin_unlock(session, &dhandle->close_lock);
			WT_RET(ret);
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
    WT_SESSION_IMPL *session, const char *name, int force)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint64_t bucket;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));
	WT_ASSERT(session, session->dhandle == NULL);

	bucket = __wt_hash_city64(name, strlen(name)) % WT_HASH_ARRAY_SIZE;
	SLIST_FOREACH(dhandle, &conn->dhhash[bucket], l) {
		if (strcmp(dhandle->name, name) != 0)
			continue;

		session->dhandle = dhandle;

		/* Lock the handle exclusively. */
		WT_ERR(__wt_session_get_btree(session,
		    dhandle->name, dhandle->checkpoint,
		    NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));
		if (WT_META_TRACKING(session))
			WT_ERR(__wt_meta_track_handle_lock(session, 0));

		/*
		 * We have an exclusive lock, which means there are no cursors
		 * open at this point.  Close the handle, if necessary.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			if ((ret = __wt_meta_track_sub_on(session)) == 0)
				ret = __wt_conn_btree_sync_and_close(
				    session, force);

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
__conn_dhandle_remove(WT_SESSION_IMPL *session, int final)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	uint64_t bucket;

	conn = S2C(session);
	dhandle = session->dhandle;
	bucket = dhandle->name_hash % WT_HASH_ARRAY_SIZE;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));

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
__wt_conn_dhandle_discard_single(WT_SESSION_IMPL *session, int final)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int tret;

	dhandle = session->dhandle;

	if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
		tret = __wt_conn_btree_sync_and_close(session, 0);
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
	F_SET(S2C(session)->cache, WT_CACHE_CLEAR_WALKS);

	/* Try to remove the handle, protected by the data handle lock. */
	WT_WITH_DHANDLE_LOCK(session,
	    WT_TRET(__conn_dhandle_remove(session, final)));

	/*
	 * After successfully removing the handle, clean it up.
	 */
	if (ret == 0 || final) {
		WT_TRET(__wt_rwlock_destroy(session, &dhandle->rwlock));
		__wt_free(session, dhandle->name);
		__wt_free(session, dhandle->checkpoint);
		__conn_btree_config_clear(session);
		__wt_free(session, dhandle->handle);
		__wt_spin_destroy(session, &dhandle->close_lock);
		__wt_overwrite_and_free(session, dhandle);

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
	 * Close open data handles: first, everything but the metadata file
	 * (as closing a normal file may open and write the metadata file),
	 * then the metadata file.  This function isn't called often, and I
	 * don't want to "know" anything about the metadata file's position on
	 * the list, so we do it the hard way.
	 */
restart:
	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		if (WT_IS_METADATA(dhandle))
			continue;

		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_conn_dhandle_discard_single(session, 1)));
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

	/* Close the metadata file handle. */
	while ((dhandle = SLIST_FIRST(&conn->dhlh)) != NULL)
		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_conn_dhandle_discard_single(session, 1)));

	return (ret);
}
