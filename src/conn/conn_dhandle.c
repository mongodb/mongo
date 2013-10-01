/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_dhandle_open_lock --
 *	Spin on the current data handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.  If exclusive access is
 *	requested and cannot be granted immediately, fail with EBUSY.
 */
static int
__conn_dhandle_open_lock(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = dhandle->handle;

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
		if (!LF_ISSET(WT_DHANDLE_EXCLUSIVE) &&
		    F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))
			return (EBUSY);

		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    !LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
			WT_RET(__wt_readlock(session, dhandle->rwlock));
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
				return (0);
			WT_RET(__wt_rwunlock(session, dhandle->rwlock));
		}

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
			    !LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
				WT_RET(__wt_rwunlock(session, dhandle->rwlock));
				continue;
			}

			/* We have an exclusive lock, we're done. */
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
			return (0);
		} else if (ret != EBUSY || LF_ISSET(WT_DHANDLE_EXCLUSIVE))
			return (EBUSY);

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}
}

/*
 * __conn_dhandle_get --
 *	Find an open btree file handle, otherwise create a new one, lock it
 * exclusively, and return it linked into the connection's list.
 */
static int
__conn_dhandle_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	uint64_t hash;

	conn = S2C(session);

	/* We must be holding the schema lock at a higher level. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/* Increment the reference count if we already have the btree open. */
	hash = __wt_hash_city64(name, strlen(name));
	SLIST_FOREACH(dhandle, &conn->dhlh, l)
		if ((hash == dhandle->name_hash &&
		     strcmp(name, dhandle->name) == 0) &&
		    ((ckpt == NULL && dhandle->checkpoint == NULL) ||
		    (ckpt != NULL && dhandle->checkpoint != NULL &&
		    strcmp(ckpt, dhandle->checkpoint) == 0))) {
			WT_RET(__conn_dhandle_open_lock(
			    session, dhandle, flags));
			++dhandle->refcnt;
			session->dhandle = dhandle;
			return (0);
		}

	/*
	 * Allocate the data source handle and underlying btree handle, then
	 * initialize the data source handle.  Exclusively lock the data
	 * source handle before inserting it in the list.
	 */
	WT_RET(__wt_calloc_def(session, 1, &dhandle));

	WT_ERR(__wt_rwlock_alloc(session, "btree handle", &dhandle->rwlock));
	dhandle->refcnt = 1;

	dhandle->name_hash = hash;
	WT_ERR(__wt_strdup(session, name, &dhandle->name));
	if (ckpt != NULL)
		WT_ERR(__wt_strdup(session, ckpt, &dhandle->checkpoint));

	WT_ERR(__wt_calloc_def(session, 1, &btree));
	dhandle->handle = btree;
	btree->dhandle = dhandle;

	F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
	WT_ERR(__wt_writelock(session, dhandle->rwlock));

	/*
	 * Prepend the handle to the connection list, assuming we're likely to
	 * need new files again soon, until they are cached by all sessions.
	 *
	 * !!!
	 * We hold only the schema lock here, not the dhandle lock.  Eviction
	 * walks this list only holding the dhandle lock.  This works because
	 * we're inserting at the beginning of the list, and we're only
	 * publishing this one entry per lock acquisition.  Eviction either
	 * sees our newly added entry or the former head of the list, and it
	 * doesn't matter which (if eviction only sees a single element in the
	 * list because the insert races, it will return without finding enough
	 * candidates for eviction, and will then retry).
	 */
	SLIST_INSERT_HEAD(&conn->dhlh, dhandle, l);

	session->dhandle = dhandle;
	return (0);

err:	if (dhandle->rwlock != NULL)
		WT_TRET(__wt_rwlock_destroy(session, &dhandle->rwlock));
	__wt_free(session, dhandle->name);
	__wt_free(session, dhandle->checkpoint);
	__wt_free(session, dhandle->handle);		/* btree free */
	__wt_overwrite_and_free(session, dhandle);

	return (ret);
}

/*
 * __wt_conn_btree_sync_and_close --
 *	Sync and close the underlying btree handle.
 */
int
__wt_conn_btree_sync_and_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int ckpt_lock;

	dhandle = session->dhandle;
	btree = S2BT(session);

	if (!F_ISSET(dhandle, WT_DHANDLE_OPEN))
		return (0);

	if (dhandle->checkpoint == NULL)
		--S2C(session)->open_btree_count;

	/*
	 * Checkpoint to flush out the file's changes.  This usually happens on
	 * session handle close (which means we're holding the handle lock, so
	 * this call serializes with any session checkpoint).  Bulk-cursors are
	 * a special case: they do not hold the handle lock and they still must
	 * serialize with checkpoints.   Acquire the lower-level checkpoint lock
	 * and hold it until the handle is closed and the bulk-cursor flag has
	 * been cleared.
	 *    We hold the lock so long for two reasons: first, checkpoint uses
	 * underlying btree handle structures (for example, the meta-tracking
	 * checkpoint resolution uses the block-manager reference), and because
	 * checkpoint writes "fake" checkpoint records for bulk-loaded files,
	 * and a real checkpoint, which we're creating here, can't be followed
	 * by more fake checkpoints.  In summary, don't let a checkpoint happen
	 * unless all of the bulk cursor's information has been cleared.
	 */
	ckpt_lock = 0;
	if (F_ISSET(btree, WT_BTREE_BULK)) {
		ckpt_lock = 1;
		__wt_spin_lock(session, &S2C(session)->checkpoint_lock);
	}

	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		ret = __wt_checkpoint_close(session, NULL);

	WT_TRET(__wt_btree_close(session));
	F_CLR(dhandle, WT_DHANDLE_OPEN);
	F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);

	if (ckpt_lock)
		__wt_spin_unlock(session, &S2C(session)->checkpoint_lock);

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
	const char *metaconf;

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
	metaconf = NULL;
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
	WT_SESSION_IMPL *session, const char *op_cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = session->dhandle;
	btree = S2BT(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) &&
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
	    !LF_ISSET(WT_DHANDLE_LOCK_ONLY));

	/*
	 * If the handle is already open, it has to be closed so it can be
	 * reopened with a new configuration.  We don't need to check again:
	 * this function isn't called if the handle is already open in the
	 * required mode.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session));

	/* Discard any previous configuration, set up the new configuration. */
	__conn_btree_config_clear(session);
	WT_RET(__conn_btree_config_set(session));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

	do {
		WT_ERR(__wt_btree_open(session, op_cfg));
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
			WT_ERR(__wt_rwunlock(session, dhandle->rwlock));
			WT_ERR(
			    __conn_dhandle_open_lock(session, dhandle, flags));
		}
	} while (!F_ISSET(dhandle, WT_DHANDLE_OPEN));

	if (0) {
err:		F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
		WT_TRET(__wt_conn_btree_close(session, 1));
	}

	return (ret);
}

/*
 * __conn_dhandle_sweep --
 *	Close and clean up any unused dhandles on the connection dhandle list.
 *	We hold a spin lock to coordinate walking this list with eviction.
 *	Put unused dhandles on a private list and unlock for eviction.
 */
static int
__conn_dhandle_sweep(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *dhandle_next, *save_dhandle;
	SLIST_HEAD(__wt_dhtmp_lh, __wt_data_handle) sweeplh;
	WT_DECL_RET;

	conn = S2C(session);
	/*
	 * Coordinate with eviction or other threads sweeping.  If the lock
	 * is not free, we're done.  Cleaning up the list is a best effort only.
	 */
	if (__wt_spin_trylock(session, &conn->dhandle_lock) != 0) {
		WT_CSTAT_INCR(session, dh_sweep_evict);
		return (0);
	}
	/*
	 * Move dead items off the list onto a local list and unlock the
	 * lock so that eviction can be unblocked.
	 */
	SLIST_INIT(&sweeplh);
	dhandle = SLIST_FIRST(&conn->dhlh);
	while (dhandle != NULL) {
		dhandle_next = SLIST_NEXT(dhandle, l);
		if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    dhandle->refcnt == 0) {
			WT_CSTAT_INCR(session, dh_conn_handles);
			SLIST_REMOVE(&conn->dhlh, dhandle, __wt_data_handle, l);
			SLIST_INSERT_HEAD(&sweeplh, dhandle, l);
		}
		dhandle = dhandle_next;
	}
	conn->dhandle_dead = 0;
	__wt_spin_unlock(session, &conn->dhandle_lock);
	/*
	 * Now actually clean up any dead handles.
	 */
	while ((dhandle = SLIST_FIRST(&sweeplh)) != NULL) {
		SLIST_REMOVE(&sweeplh, dhandle, __wt_data_handle, l);
		/*
		 * Record any errors, but still discard all of them.
		 * This call will clear the session dhandle field.  Save
		 * the value and restore it after it returns.
		 */
		save_dhandle = session->dhandle;
		WT_TRET(__wt_conn_dhandle_discard_single(session, dhandle));
		session->dhandle = save_dhandle;
	}
	return (ret);
}

/*
 * __wt_conn_btree_get --
 *	Get an open btree file handle, otherwise open a new one.
 */
int
__wt_conn_btree_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, const char *op_cfg[], uint32_t flags)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	if (S2C(session)->dhandle_dead >= CONN_DHANDLE_SWEEP_TRIGGER)
		WT_RET(__conn_dhandle_sweep(session));
	WT_RET(__conn_dhandle_get(session, name, ckpt, flags));
	dhandle = session->dhandle;

	if (!LF_ISSET(WT_DHANDLE_LOCK_ONLY) &&
	    (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
	    LF_ISSET(WT_BTREE_SPECIAL_FLAGS)))
		if ((ret = __conn_btree_open(session, op_cfg, flags)) != 0) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			WT_TRET(__wt_rwunlock(session, dhandle->rwlock));
		}

	WT_ASSERT(session, ret != 0 ||
	    LF_ISSET(WT_DHANDLE_EXCLUSIVE) ==
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));

	return (ret);
}

/*
 * __wt_conn_btree_apply --
 *	Apply a function to all open btree handles apart from the metadata
 * file.
 */
int
__wt_conn_btree_apply(WT_SESSION_IMPL *session,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	SLIST_FOREACH(dhandle, &conn->dhlh, l)
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    WT_PREFIX_MATCH(dhandle->name, "file:") &&
		    !WT_IS_METADATA(dhandle)) {
			/*
			 * We need to pull the handle into the session handle
			 * cache and make sure it's referenced to stop other
			 * internal code dropping the handle (e.g in LSM when
			 * cleaning up obsolete chunks). Holding the metadata
			 * lock isn't enough.
			 */
			ret = __wt_session_get_btree(session,
			    dhandle->name, dhandle->checkpoint, NULL, 0);
			if (ret == 0) {
				ret = func(session, cfg);
				if (WT_META_TRACKING(session))
					WT_TRET(__wt_meta_track_handle_lock(
					    session, 0));
				else
					WT_TRET(__wt_session_release_btree(
					    session));
			} else if (ret == EBUSY)
				ret = __wt_conn_btree_apply_single(
				    session, dhandle->name,
				    dhandle->checkpoint, func, cfg);
			WT_RET(ret);
		}

	return (0);
}

/*
 * __wt_conn_btree_apply_single --
 *	Apply a function to a single btree handle that's being bulk-loaded.
 */
int
__wt_conn_btree_apply_single(WT_SESSION_IMPL *session,
    const char *uri, const char *checkpoint,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_DECL_RET;

	conn = S2C(session);
	saved_dhandle = session->dhandle;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	SLIST_FOREACH(dhandle, &conn->dhlh, l)
		if (strcmp(dhandle->name, uri) == 0 &&
		    ((dhandle->checkpoint == NULL && checkpoint == NULL) ||
		    (dhandle->checkpoint != NULL && checkpoint != NULL &&
		    strcmp(dhandle->checkpoint, checkpoint) == 0))) {
			/*
			 * We have the schema lock, which prevents handles being
			 * opened or closed, so there is no need for additional
			 * handle locking here, or pulling every tree into this
			 * session's handle cache.
			 *
			 * XXX
			 * THIS IS NOT CORRECT.  This is safe for checkpoints
			 * because LSM (which closes handles without holding
			 * the schema lock), does hold the checkpoint lock as
			 * it discards those handles.  This isn't safe for
			 * hot-backup (or other callers of this function) as
			 * they are only holding the schema lock.  The problem
			 * is vanishingly unlikely to happen, but it needs to
			 * be fixed.
			 *
			 * We're holding the schema lock, which means the only
			 * files we are going to be asked for are bulk-load
			 * files.  Other code that might cause an EBUSY return
			 * to our caller (for example, WT_SESSION.verify or
			 * drop), will itself be holding the schema lock, and
			 * will serialize with our caller.  In summary, if the
			 * handle requested wasn't for a bulk-load file, it's
			 * all gone pear-shaped.
			 */
			if (F_ISSET((WT_BTREE *)(dhandle->handle),
			    WT_BTREE_SPECIAL_FLAGS) == WT_BTREE_BULK) {
				session->dhandle = dhandle;
				WT_ERR(func(session, cfg));
			} else
				WT_ERR(EBUSY);
		}

err:	session->dhandle = saved_dhandle;
	return (ret);
}

/*
 * __wt_conn_btree_close --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_btree_close(WT_SESSION_IMPL *session, int locked)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int inuse;

	dhandle = session->dhandle;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Decrement the reference count.  If we really are the last reference,
	 * get an exclusive lock on the handle so that we can close it.
	 */
	inuse = --dhandle->refcnt > 0;
	if (!inuse && !locked) {
		/*
		 * XXX
		 * If we fail to get the lock it should be OK (the reference
		 * count has already been decremented), but it's really not a
		 * good thing.
		 */
		WT_RET(__wt_writelock(session, dhandle->rwlock));
		F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
	}

	if (!inuse) {
		/*
		 * We should only close the metadata file when closing the
		 * last session (i.e., the default session for the connection)
		 * or at the end of recovery.
		 */
		WT_ASSERT(session,
		    S2BT(session) != session->metafile ||
		    session == S2C(session)->default_session ||
		    F_ISSET(session, WT_SESSION_LOGGING_DISABLED));

		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			WT_TRET(__wt_conn_btree_sync_and_close(session));
			S2C(session)->dhandle_dead++;
		}
		if (!locked) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			WT_TRET(__wt_rwunlock(session, dhandle->rwlock));
		}
	}

	return (ret);
}

/*
 * __wt_conn_dhandle_close_all --
 *	Close all data handles handles with matching name (including all
 *	checkpoint handles).
 */
int
__wt_conn_dhandle_close_all(WT_SESSION_IMPL *session, const char *name)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_DECL_RET;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Make sure the caller's handle is tracked, so it will be unlocked
	 * even if we failed to get all of the remaining handles we need.
	 */
	if ((saved_dhandle = session->dhandle) != NULL &&
	    WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_handle_lock(session, 0));

	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		if (strcmp(dhandle->name, name) != 0)
			continue;

		session->dhandle = dhandle;

		/*
		 * The caller may have this tree locked to prevent
		 * concurrent schema operations.
		 */
		if (dhandle == saved_dhandle)
			WT_ASSERT(session,
			    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));
		else {
			WT_ERR(__wt_try_writelock(session, dhandle->rwlock));
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
			if (WT_META_TRACKING(session))
				WT_ERR(__wt_meta_track_handle_lock(session, 0));
		}

		/*
		 * We have an exclusive lock, which means there are no
		 * cursors open at this point.  Close the handle, if
		 * necessary.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			ret = __wt_meta_track_sub_on(session);
			if (ret == 0)
				ret = __wt_conn_btree_sync_and_close(session);

			/*
			 * If the close succeeded, drop any locks it
			 * acquired.  If there was a failure, this
			 * function will fail and the whole transaction
			 * will be rolled back.
			 */
			if (ret == 0)
				ret = __wt_meta_track_sub_off(session);
		}

		if (!WT_META_TRACKING(session))
			WT_TRET(__wt_session_release_btree(session));

		session->dhandle = NULL;
		WT_ERR(ret);
	}

err:	session->dhandle = NULL;
	return (ret);
}

/*
 * __wt_conn_dhandle_discard_single --
 *	Discard a single data handle structure.
 */
int
__wt_conn_dhandle_discard_single(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle)
{
	WT_DECL_RET;

	session->dhandle = dhandle;

	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_TRET(__wt_conn_btree_sync_and_close(session));

	WT_TRET(__wt_rwlock_destroy(session, &dhandle->rwlock));
	__wt_free(session, dhandle->name);
	__wt_free(session, dhandle->checkpoint);
	__conn_btree_config_clear(session);
	__wt_free(session, dhandle->handle);
	__wt_overwrite_and_free(session, dhandle);

	WT_CLEAR_BTREE_IN_SESSION(session);

	return (ret);
}

/*
 * __wt_conn_dhandle_discard --
 *	Discard the btree file handle structures.
 */
int
__wt_conn_dhandle_discard(WT_CONNECTION_IMPL *conn)
{
	WT_DATA_HANDLE *dhandle;
	WT_DATA_HANDLE_CACHE *dhandle_cache;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* 
	 * Close is only sort of single-threaded, we should not conflict
	 * on the schema lock with other running threads, but we need to
	 * have the dhandle lock because the eviction thread is still
	 * running and may walk the dhandle list.
	 */
	F_SET(session, WT_SESSION_SCHEMA_LOCKED);
	__wt_spin_lock(session, &conn->schema_lock);
	__wt_spin_lock(session, &conn->dhandle_lock);

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

		SLIST_REMOVE(&conn->dhlh, dhandle, __wt_data_handle, l);
		WT_TRET(__wt_conn_dhandle_discard_single(session, dhandle));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our session's list
	 * of open data handles, specifically, we added the metadata file if
	 * any of the files were dirty.  Clean up that list before we shut down
	 * the metadata entry, for good.
	 */
	while ((dhandle_cache = SLIST_FIRST(&session->dhandles)) != NULL)
		WT_TRET(__wt_session_discard_btree(session, dhandle_cache));

	/* Close the metadata file handle. */
	while ((dhandle = SLIST_FIRST(&conn->dhlh)) != NULL) {
		SLIST_REMOVE(&conn->dhlh, dhandle, __wt_data_handle, l);
		WT_TRET(__wt_conn_dhandle_discard_single(session, dhandle));
	}
	__wt_spin_unlock(session, &conn->dhandle_lock);
	__wt_spin_unlock(session, &conn->schema_lock);

	return (ret);
}
