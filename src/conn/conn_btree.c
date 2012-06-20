/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_btree_open_lock --
 *	Spin on the current btree handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.
 */
void
__wt_conn_btree_open_lock(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;

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
		if (F_ISSET(btree, WT_BTREE_OPEN) &&
		    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			__wt_readlock(session, btree->rwlock);
			if (F_ISSET(btree, WT_BTREE_OPEN))
				break;
			__wt_rwunlock(session, btree->rwlock);
		}

		/*
		 * It isn't open or we want it exclusive: try to get an
		 * exclusive lock.  There is some subtlety here: if we race
		 * with another thread that successfully opens the file, we
		 * don't want to block waiting to get exclusive access.
		 */
		if (__wt_try_writelock(session, btree->rwlock) == 0) {
			/*
			 * If it was opened while we waited, drop the write
			 * lock and get a read lock instead.
			 */
			if (F_ISSET(btree, WT_BTREE_OPEN) &&
			    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
				__wt_rwunlock(session, btree->rwlock);
				continue;
			}

			/* We have an exclusive lock, we're done. */
			F_SET(btree, WT_BTREE_EXCLUSIVE);
			break;
		}

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}
}

/*
 * __conn_btree_get --
 *	Find an open btree file handle, otherwise create a new one and link it
 *	into the connection's list.  If successful, it returns with either
 *	(a) an open handle, read locked (if WT_BTREE_EXCLUSIVE is set); or
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
	int matched;

	conn = S2C(session);

	/*
	 * If we aren't holding the connection spinlock at a higher level,
	 * acquire it now.
	 */
	if (!F_ISSET(session, WT_SESSION_HAS_CONNLOCK))
		__wt_spin_lock(session, &conn->spinlock);

	/* Increment the reference count if we already have the btree open. */
	matched = 0;
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(name, btree->name) == 0 &&
		    ((ckpt == NULL && btree->checkpoint == NULL) ||
		    (ckpt != NULL && btree->checkpoint != NULL &&
		    strcmp(ckpt, btree->checkpoint) == 0))) {
			++btree->refcnt;
			session->btree = btree;
			matched = 1;
			break;
		}
	}
	if (matched) {
		if (!F_ISSET(session, WT_SESSION_HAS_CONNLOCK))
			__wt_spin_unlock(session, &conn->spinlock);
		__wt_conn_btree_open_lock(session, flags);
		return (0);
	}

	/*
	 * Allocate the WT_BTREE structure, its lock, and set the name so we
	 * can put the handle into the list.
	 */
	btree = NULL;
	if ((ret = __wt_calloc_def(session, 1, &btree)) == 0 &&
	    (ret = __wt_rwlock_alloc(
		session, "btree handle", &btree->rwlock)) == 0 &&
	    (ret = __wt_strdup(session, name, &btree->name)) == 0 &&
	    (ckpt == NULL ||
	    (ret = __wt_strdup(session, ckpt, &btree->checkpoint)) == 0)) {
		/* Lock the handle before it is inserted in the list. */
		__wt_writelock(session, btree->rwlock);
		F_SET(btree, WT_BTREE_EXCLUSIVE);

		/* Add to the connection list. */
		btree->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->btqh, btree, q);
		++conn->btqcnt;
	}

	if (!F_ISSET(session, WT_SESSION_HAS_CONNLOCK))
		__wt_spin_unlock(session, &conn->spinlock);

	if (ret == 0)
		session->btree = btree;
	else if (btree != NULL) {
		if (btree->rwlock != NULL)
			__wt_rwlock_destroy(session, &btree->rwlock);
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
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = session->btree;

	if (!F_ISSET(btree, WT_BTREE_OPEN))
		return (0);

	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		ret = __wt_checkpoint(session, NULL);

	WT_TRET(__wt_btree_close(session));

	F_CLR(btree,
	    WT_BTREE_OPEN | WT_BTREE_NO_EVICTION | WT_BTREE_SPECIAL_FLAGS);

	return (ret);
}

/*
 * __wt_conn_btree_open --
 *	Open the current btree handle.
 */
int
__wt_conn_btree_open(WT_SESSION_IMPL *session,
    const char *config, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(addr);
	WT_DECL_RET;

	btree = session->btree;

	WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE) &&
	    !LF_ISSET(WT_BTREE_LOCK_ONLY));

	/* Open the underlying file, free any old config. */
	__wt_free(session, btree->config);
	btree->config = config;

	/*
	 * If the handle is already open, it has to be closed so it can be
	 * reopened with a new configuration.  We don't need to check again:
	 * this function isn't called if the handle is already open in the
	 * required mode.
	 */
	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session));

	WT_RET(__wt_scr_alloc(
	    session, WT_BTREE_MAX_ADDR_COOKIE, &addr));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

	/* The metadata file is never evicted. */
	if (strcmp(btree->name, WT_METADATA_URI) == 0)
		F_SET(btree, WT_BTREE_NO_EVICTION);

	do {
		WT_ERR(__wt_meta_checkpoint_get(
		    session, btree->name, btree->checkpoint, addr));
		WT_ERR(__wt_btree_open(session, addr->data, addr->size, cfg,
		    btree->checkpoint == NULL ? 0 : 1));
		F_SET(btree, WT_BTREE_OPEN);

		/* Drop back to a readlock if that is all that was needed. */
		if (!LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			F_CLR(btree, WT_BTREE_EXCLUSIVE);
			__wt_rwunlock(session, btree->rwlock);
			__wt_conn_btree_open_lock(session, flags);
		}
	} while (!F_ISSET(btree, WT_BTREE_OPEN));

	if (0) {
err:		(void)__wt_conn_btree_close(session, 1);
	}

	__wt_scr_free(&addr);
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
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *treeconf;
	int locked;

	conn = S2C(session);

	WT_STAT_INCR(conn->stats, file_open);

	locked = 1;
	if ((btree = session->btree) != NULL) {
		if (!F_ISSET(btree, WT_BTREE_EXCLUSIVE))
			__wt_conn_btree_open_lock(session, flags);
		else
			locked = 0;
	} else {
		WT_RET(__conn_btree_get(session, name, ckpt, flags));
		btree = session->btree;
	}

	if (!LF_ISSET(WT_BTREE_LOCK_ONLY) &&
	    (!F_ISSET(session->btree, WT_BTREE_OPEN) ||
	    LF_ISSET(WT_BTREE_SPECIAL_FLAGS))) {
		if ((ret = __wt_metadata_read(session, name, &treeconf)) != 0) {
			if (ret == WT_NOTFOUND)
				ret = ENOENT;
			goto err;
		}
		ret = __wt_conn_btree_open(session, treeconf, cfg, flags);
	}

err:	if (ret != 0 && locked) {
		F_CLR(btree, WT_BTREE_EXCLUSIVE);
		__wt_rwunlock(session, btree->rwlock);
	}

	WT_ASSERT(session, ret != 0 ||
	    LF_ISSET(WT_BTREE_EXCLUSIVE) == F_ISSET(btree, WT_BTREE_EXCLUSIVE));

	return (ret);
}

/*
 * __wt_conn_btree_apply --
 *	Apply a function to all open, non-checkpoint btree handles apart from
 * the metadata file.
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

	__wt_spin_lock(session, &conn->spinlock);
	F_SET(session, WT_SESSION_HAS_CONNLOCK);
	TAILQ_FOREACH(btree, &conn->btqh, q)
		if (F_ISSET(btree, WT_BTREE_OPEN) &&
		    btree->checkpoint == NULL &&
		    strcmp(btree->name, WT_METADATA_URI) != 0) {
			/*
			 * We have the connection spinlock, which prevents
			 * handles being opened or closed, so there is no need
			 * for additional handle locking here, or pulling every
			 * tree into this session's handle cache.
			 */
			session->btree = btree;
			WT_ERR(func(session, cfg));
		}

err:	F_CLR(session, WT_SESSION_HAS_CONNLOCK);
	__wt_spin_unlock(session, &conn->spinlock);
	session->btree = saved_btree;
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

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_HAS_CONNLOCK));

	/*
	 * Decrement the reference count.  If we really are the last reference,
	 * get an exclusive lock on the handle so that we can close it.
	 */
	__wt_spin_lock(session, &conn->spinlock);
	inuse = --btree->refcnt > 0;
	if (!inuse && !locked) {
		__wt_writelock(session, btree->rwlock);
		F_SET(btree, WT_BTREE_EXCLUSIVE);
	}
	__wt_spin_unlock(session, &conn->spinlock);

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
			__wt_rwunlock(session, btree->rwlock);
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
	saved_btree = session->btree;

	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_HAS_CONNLOCK));

	__wt_spin_lock(session, &conn->spinlock);
	F_SET(session, WT_SESSION_HAS_CONNLOCK);
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(btree->name, name) != 0)
			continue;

		/*
		 * The caller may have this tree locked to prevent
		 * concurrent schema operations.
		 */
		if (btree == saved_btree)
			WT_ASSERT(session, F_ISSET(btree, WT_BTREE_EXCLUSIVE));
		else {
			WT_ERR(__wt_try_writelock(session, btree->rwlock));
			F_SET(btree, WT_BTREE_EXCLUSIVE);
		}

		session->btree = btree;
		if (WT_META_TRACKING(session))
			WT_ERR(__wt_meta_track_handle_lock(session));

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
		session->btree = NULL;

		WT_ERR(ret);
	}

err:	F_CLR(session, WT_SESSION_HAS_CONNLOCK);
	__wt_spin_unlock(session, &conn->spinlock);
	return (ret);
}

/*
 * __conn_btree_discard --
 *	Discard a single btree file handle structure.
 */
static int
__conn_btree_discard(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	WT_DECL_RET;

	if (F_ISSET(btree, WT_BTREE_OPEN)) {
		WT_SET_BTREE_IN_SESSION(session, btree);
		WT_TRET(__wt_conn_btree_sync_and_close(session));
		WT_CLEAR_BTREE_IN_SESSION(session);
	}
	__wt_rwlock_destroy(session, &btree->rwlock);
	__wt_free(session, btree->config);
	__wt_free(session, btree->name);
	__wt_free(session, btree->checkpoint);
	__wt_overwrite_and_free(session, btree);

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
		--conn->btqcnt;
		WT_TRET(__conn_btree_discard(session, btree));
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
		--conn->btqcnt;
		WT_TRET(__conn_btree_discard(session, btree));
	}

	return (ret);
}
