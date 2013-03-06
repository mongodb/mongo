/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_btree_open_lock --
 *	Spin on the current btree handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.  If exclusive access is
 *	requested and cannot be granted immediately, fail with EBUSY.
 */
static int
__conn_btree_open_lock(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;

	/*
	 * Check that the handle is open.  We've already incremented
	 * the reference count, so once the handle is open it won't be
	 * closed by another thread.
	 *
	 * If we can see the WT_BTREE_OPEN flag set while holding a
	 * lock on the handle, then it's really open and we can start
	 * using it.  Alternatively, if we can get an exclusive lock
	 * and WT_BTREE_OPEN is still not set, we need to do the open.
	 */
	for (;;) {
		if (!LF_ISSET(WT_BTREE_EXCLUSIVE) &&
		    F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))
			return (EBUSY);

		if (F_ISSET(btree, WT_BTREE_OPEN) &&
		    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			WT_RET(__wt_readlock(session, btree->rwlock));
			if (F_ISSET(btree, WT_BTREE_OPEN))
				return (0);
			WT_RET(__wt_rwunlock(session, btree->rwlock));
		}

		/*
		 * It isn't open or we want it exclusive: try to get an
		 * exclusive lock.  There is some subtlety here: if we race
		 * with another thread that successfully opens the file, we
		 * don't want to block waiting to get exclusive access.
		 */
		if ((ret = __wt_try_writelock(session, btree->rwlock)) == 0) {
			/*
			 * If it was opened while we waited, drop the write
			 * lock and get a read lock instead.
			 */
			if (F_ISSET(btree, WT_BTREE_OPEN) &&
			    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
				WT_RET(__wt_rwunlock(session, btree->rwlock));
				continue;
			}

			/* We have an exclusive lock, we're done. */
			F_SET(btree, WT_BTREE_EXCLUSIVE);
			return (0);
		}
		if (ret != EBUSY || LF_ISSET(WT_BTREE_EXCLUSIVE))
			return (ret);

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}
}

/*
 * __conn_btree_get --
 *	Find an open btree file handle, otherwise create a new one and link it
 *	into the connection's list.  If successful, it returns with either
 *	(a) an open handle, read locked (if WT_BTREE_EXCLUSIVE is not set); or
 *	(b) an open handle, write locked (if WT_BTREE_EXCLUSIVE is set), or
 *	(c) a closed handle, write locked.
 */
static int
__conn_btree_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	/* We must be holding the schema lock at a higher level. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/* Increment the reference count if we already have the btree open. */
	TAILQ_FOREACH(btree, &conn->btqh, q)
		if (strcmp(name, btree->name) == 0 &&
		    ((ckpt == NULL && btree->checkpoint == NULL) ||
		    (ckpt != NULL && btree->checkpoint != NULL &&
		    strcmp(ckpt, btree->checkpoint) == 0))) {
			++btree->refcnt;
			session->btree = btree;
			return (__conn_btree_open_lock(session, flags));
		}

	/*
	 * Allocate the WT_BTREE structure, its lock, and set the name so we
	 * can add the handle to the list.  Lock the handle before inserting
	 * it in the list.
	 */
	btree = NULL;
	if ((ret = __wt_calloc_def(session, 1, &btree)) == 0 &&
	    (ret = __wt_rwlock_alloc(
		session, "btree handle", &btree->rwlock)) == 0 &&
	    (ret = __wt_strdup(session, name, &btree->name)) == 0 &&
	    (ckpt == NULL ||
	    (ret = __wt_strdup(session, ckpt, &btree->checkpoint)) == 0) &&
	    (ret = __wt_writelock(session, btree->rwlock)) == 0) {
		F_SET(btree, WT_BTREE_EXCLUSIVE);

		/* Add to the connection list. */
		btree->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->btqh, btree, q);
	}

	if (ret == 0)
		session->btree = btree;
	else if (btree != NULL) {
		if (btree->rwlock != NULL)
			WT_TRET(__wt_rwlock_destroy(session, &btree->rwlock));
		__wt_free(session, btree->name);
		__wt_free(session, btree->checkpoint);
		__wt_overwrite_and_free(session, btree);
	}

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
	WT_DECL_RET;
	int ckpt_lock;

	btree = session->btree;

	if (!F_ISSET(btree, WT_BTREE_OPEN))
		return (0);

	if (btree->checkpoint == NULL)
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
		__wt_spin_lock(session, &S2C(session)->metadata_lock);
	}

	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		ret = __wt_checkpoint_close(session, NULL);

	WT_TRET(__wt_btree_close(session));
	F_CLR(btree, WT_BTREE_OPEN | WT_BTREE_SPECIAL_FLAGS);

	if (ckpt_lock)
		__wt_spin_unlock(session, &S2C(session)->metadata_lock);

	return (ret);
}

/*
 * __conn_btree_config_clear --
 *	Clear the underlying object's configuration information.
 */
static void
__conn_btree_config_clear(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	const char **a;

	btree = session->btree;

	if (btree->cfg == NULL)
		return;
	for (a = btree->cfg; *a != NULL; ++a)
		__wt_free(session, *a);
	__wt_free(session, btree->cfg);
}

/*
 * __conn_btree_config_set --
 *	Set up the underlying object's configuration information.
 */
static int
__conn_btree_config_set(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_DECL_RET;
	u_int i;
	const char **a, **b, *metaconf;

	btree = session->btree;

	/*
	 * Read the object's entry from the metadata file, we're done if we
	 * don't find one.
	 */
	if ((ret = __wt_metadata_read(session, btree->name, &metaconf)) != 0) {
		if (ret == WT_NOTFOUND)
			ret = ENOENT;
		WT_RET(ret);
	}

	/*
	 * The configuration information in descending order of importance: the
	 * defaults, the metadata, then the specific open call's configuration.
	 *
	 * The defaults are included because underlying objects have persistent
	 * configuration information stored in the metadata file.  If defaults
	 * are included in the configuration, we can add new configuration
	 * strings without upgrading the metadata file or writing special code
	 * in case a configuration string isn't initialized, as long as the new
	 * configuration string has an appropriate default value.
	 *
	 * Allocate the object's configuration array.
	 *
	 * The error handling is a little odd, but be careful: we're holding a
	 * chunk of allocated memory in metaconf.  If we fail before we copy a
	 * reference to it into the object's configuration array, we must free
	 * it, after the copy, we don't want to free it.
	 */
	i = 3;
	if (cfg != NULL)
		for (b = cfg; *b != NULL; ++b)
			++i;
	WT_ERR(__wt_calloc_def(session, i, &btree->cfg));

	a = btree->cfg;
	WT_ERR(__wt_strdup(session, __wt_confdfl_file_meta, a));
	if (metaconf != NULL) {
		*++a = metaconf;
		metaconf = NULL;
	}
	if (cfg != NULL)
		for (b = cfg; *b != NULL; ++b)
			WT_ERR(__wt_strdup(session, *b, ++a));
	return (0);

err:	__wt_free(session, metaconf);
	return (ret);
}

/*
 * __conn_btree_open --
 *	Open the current btree handle.
 */
static int
__conn_btree_open(WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) &&
	    F_ISSET(btree, WT_BTREE_EXCLUSIVE) &&
	    !LF_ISSET(WT_BTREE_LOCK_ONLY));

	/*
	 * If the handle is already open, it has to be closed so it can be
	 * reopened with a new configuration.  We don't need to check again:
	 * this function isn't called if the handle is already open in the
	 * required mode.
	 */
	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session));

	/* Discard any previous configuration, set up the new configuration. */
	__conn_btree_config_clear(session);
	WT_RET(__conn_btree_config_set(session, cfg));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

	do {
		WT_ERR(__wt_btree_open(session));
		F_SET(btree, WT_BTREE_OPEN);
		/*
		 * Checkpoint handles are read only, so eviction calculations
		 * based on the number of btrees are better to ignore them.
		 */
		if (btree->checkpoint == NULL)
			++S2C(session)->open_btree_count;

		/* Drop back to a readlock if that is all that was needed. */
		if (!LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			F_CLR(btree, WT_BTREE_EXCLUSIVE);
			WT_ERR(__wt_rwunlock(session, btree->rwlock));
			WT_ERR(__conn_btree_open_lock(session, flags));
		}
	} while (!F_ISSET(btree, WT_BTREE_OPEN));

	if (0) {
err:		F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
		WT_TRET(__wt_conn_btree_close(session, 1));
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
	WT_BTREE *btree;
	WT_DECL_RET;

	WT_RET(__conn_btree_get(session, name, ckpt, flags));

	btree = session->btree;

	if (!LF_ISSET(WT_BTREE_LOCK_ONLY) &&
	    (!F_ISSET(btree, WT_BTREE_OPEN) ||
	    LF_ISSET(WT_BTREE_SPECIAL_FLAGS)))
		if ((ret = __conn_btree_open(session, cfg, flags)) != 0) {
			F_CLR(btree, WT_BTREE_EXCLUSIVE);
			WT_TRET(__wt_rwunlock(session, btree->rwlock));
		}

	WT_ASSERT(session, ret != 0 ||
	    LF_ISSET(WT_BTREE_EXCLUSIVE) == F_ISSET(btree, WT_BTREE_EXCLUSIVE));

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
	WT_BTREE *btree, *saved_btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	saved_btree = session->btree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	TAILQ_FOREACH(btree, &conn->btqh, q)
		if (F_ISSET(btree, WT_BTREE_OPEN) &&
		    strcmp(btree->name, WT_METADATA_URI) != 0) {
			/*
			 * We have the schema lock, which prevents handles being
			 * opened or closed, so there is no need for additional
			 * handle locking here, or pulling every tree into this
			 * session's handle cache.
			 */
			session->btree = btree;
			WT_ERR(func(session, cfg));
		}

err:	session->btree = saved_btree;
	return (ret);
}

/*
 * __wt_conn_btree_apply_single --
 *	Apply a function to a single btree handle.
 */
int
__wt_conn_btree_apply_single(WT_SESSION_IMPL *session, const char *uri,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_BTREE *btree, *saved_btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	saved_btree = session->btree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	TAILQ_FOREACH(btree, &conn->btqh, q)
		if (strcmp(btree->name, uri) == 0) {
			/*
			 * We have the schema lock, which prevents handles being
			 * opened or closed, so there is no need for additional
			 * handle locking here, or pulling every tree into this
			 * session's handle cache.
			 */
			session->btree = btree;
			WT_ERR(func(session, cfg));
		}

err:	session->btree = saved_btree;
	return (ret);
}

/*
 * __wt_conn_btree_close --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_btree_close(WT_SESSION_IMPL *session, int locked)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int inuse;

	btree = session->btree;
	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Decrement the reference count.  If we really are the last reference,
	 * get an exclusive lock on the handle so that we can close it.
	 */
	inuse = --btree->refcnt > 0;
	if (!inuse && !locked) {
		/*
		 * XXX
		 * If we fail to get the lock it should be OK (the reference
		 * count has already been decremented), but it's really not a
		 * good thing.
		 */
		WT_RET(__wt_writelock(session, btree->rwlock));
		F_SET(btree, WT_BTREE_EXCLUSIVE);
	}

	if (!inuse) {
		/*
		 * We should only close the metadata file when closing the
		 * last session (i.e., the default session for the connection).
		 */
		WT_ASSERT(session,
		    btree != session->metafile ||
		    session == conn->default_session);

		if (F_ISSET(btree, WT_BTREE_OPEN))
			WT_TRET(__wt_conn_btree_sync_and_close(session));
		if (!locked) {
			F_CLR(btree, WT_BTREE_EXCLUSIVE);
			WT_TRET(__wt_rwunlock(session, btree->rwlock));
		}
	}

	return (ret);
}

/*
 * __wt_conn_btree_close_all --
 *	Close all btree handles handles with matching name (including all
 *	checkpoint handles).
 */
int
__wt_conn_btree_close_all(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE *btree, *saved_btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Make sure the caller's handle is tracked, so it will be unlocked
	 * even if we failed to get all of the remaining handles we need.
	 */
	if ((saved_btree = session->btree) != NULL && WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_handle_lock(session, 0));

	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(btree->name, name) != 0)
			continue;

		WT_SET_BTREE_IN_SESSION(session, btree);

		/*
		 * The caller may have this tree locked to prevent
		 * concurrent schema operations.
		 */
		if (btree == saved_btree)
			WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		else {
			WT_ERR(__wt_try_writelock(session, btree->rwlock));
			F_SET(btree, WT_BTREE_EXCLUSIVE);
			if (WT_META_TRACKING(session))
				WT_ERR(__wt_meta_track_handle_lock(session, 0));
		}

		/*
		 * We have an exclusive lock, which means there are no
		 * cursors open at this point.  Close the handle, if
		 * necessary.
		 */
		if (F_ISSET(btree, WT_BTREE_OPEN)) {
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

		WT_CLEAR_BTREE_IN_SESSION(session);
		WT_ERR(ret);
	}

err:	WT_CLEAR_BTREE_IN_SESSION(session);
	return (ret);
}

/*
 * __wt_conn_btree_discard_single --
 *	Discard a single btree file handle structure.
 */
int
__wt_conn_btree_discard_single(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	WT_DECL_RET;

	WT_SET_BTREE_IN_SESSION(session, btree);

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_TRET(__wt_conn_btree_sync_and_close(session));

	WT_TRET(__wt_rwlock_destroy(session, &btree->rwlock));
	__wt_free(session, btree->name);
	__wt_free(session, btree->checkpoint);
	__conn_btree_config_clear(session);
	__wt_overwrite_and_free(session, btree);

	WT_CLEAR_BTREE_IN_SESSION(session);

	return (ret);
}

/*
 * __wt_conn_btree_discard --
 *	Discard the btree file handle structures.
 */
int
__wt_conn_btree_discard(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* Close is single-threaded, no need to get the lock for real. */
	F_SET(session, WT_SESSION_SCHEMA_LOCKED);

	/*
	 * Close open btree handles: first, everything but the metadata file
	 * (as closing a normal file may open and write the metadata file),
	 * then the metadata file.  This function isn't called often, and I
	 * don't want to "know" anything about the metadata file's position on
	 * the list, so we do it the hard way.
	 */
restart:
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(btree->name, WT_METADATA_URI) == 0)
			continue;

		TAILQ_REMOVE(&conn->btqh, btree, q);
		WT_TRET(__wt_conn_btree_discard_single(session, btree));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our session's list
	 * of open btree handles, specifically, we added the metadata file if
	 * any of the files were dirty.  Clean up that list before we shut down
	 * the metadata entry, for good.
	 */
	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL)
		WT_TRET(__wt_session_discard_btree(session, btree_session));

	/* Close the metadata file handle. */
	while ((btree = TAILQ_FIRST(&conn->btqh)) != NULL) {
		TAILQ_REMOVE(&conn->btqh, btree, q);
		WT_TRET(__wt_conn_btree_discard_single(session, btree));
	}

	return (ret);
}
