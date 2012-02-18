/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_open_btree --
 *	Find an open btree file handle, otherwise create a new one and link it
 * into the connection's list.
 */
int
__wt_conn_open_btree(WT_SESSION_IMPL *session,
    const char *name, const char *filename, const char *config,
    const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	int matched, ret;

	conn = S2C(session);
	ret = 0;

	WT_STAT_INCR(conn->stats, file_open);

	/*
	 * The file configuration string must point to allocated memory: it
	 * is stored in the returned btree handle and freed when the handle
	 * is closed.
	 */

	/* Increment the reference count if we already have the btree open. */
	matched = 0;
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(filename, btree->filename) == 0) {
			++btree->refcnt;
			session->btree = btree;
			matched = 1;
			break;
		}
	}
	if (matched) {
		__wt_spin_unlock(session, &conn->spinlock);

		/* Check that the handle is open. */
		__wt_readlock(session, btree->rwlock);
		matched = F_ISSET(btree, WT_BTREE_OPEN);
		__wt_rwunlock(session, btree->rwlock);

		if (!matched) {
			__wt_writelock(session, btree->rwlock);
			if (!F_ISSET(btree, WT_BTREE_OPEN)) {
				/* We're going to overwrite the old config. */
				__wt_free(session, btree->config);
				goto conf;
			}

			/* It was opened while we waited. */
			__wt_rwunlock(session, btree->rwlock);
		}

		/* The config string will not be needed: free it now. */
		__wt_free(session, config);

		session->btree = btree;
		return (0);
	}

	/*
	 * Allocate the WT_BTREE structure, its lock, and set the name so we
	 * can put the handle into the list.
	 *
	 * Because this loop checks for existing btree file handles, the
	 * connection layer owns:
	 *	the WT_BTREE structure itself
	 *	the structure lock
	 *	the structure names
	 *	the structure configuration string
	 */
	btree = NULL;
	if ((ret = __wt_calloc_def(session, 1, &btree)) == 0 &&
	    (ret = __wt_rwlock_alloc(
		session, "btree handle", &btree->rwlock)) == 0 &&
	    (ret = __wt_strdup(session, name, &btree->name)) == 0 &&
	    (ret = __wt_strdup(session, filename, &btree->filename)) == 0) {
		/* Lock the handle before it is inserted in the list. */
		__wt_writelock(session, btree->rwlock);

		/* Add to the connection list. */
		btree->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->btqh, btree, q);
		++conn->btqcnt;
	}
	__wt_spin_unlock(session, &conn->spinlock);

	if (ret != 0) {
		if (btree != NULL) {
			if (btree->rwlock != NULL)
				(void)__wt_rwlock_destroy(
				    session, btree->rwlock);
			__wt_free(session, btree->filename);
			__wt_free(session, btree->name);
			__wt_free(session, btree);
			__wt_free(session, config);
		}
		return (ret);
	}

	/* Open the underlying file. */
conf:	session->btree = btree;
	btree->config = config;

	ret = __wt_btree_open(session, cfg, flags);
	__wt_rwunlock(session, btree->rwlock);
	if (ret == 0)
		return (0);

	(void)__wt_conn_close_btree(session);
	return (ret);
}

/*
 * __wt_conn_close_btree --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_close_btree(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	int inuse, ret;

	btree = session->btree;
	conn = S2C(session);
	ret = 0;

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	/* Remove from the connection's list. */
	__wt_spin_lock(session, &conn->spinlock);
	inuse = (--btree->refcnt > 0);
	if (!inuse) {
		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
	}
	__wt_spin_unlock(session, &conn->spinlock);

	if (inuse)
		return (0);

	ret = __wt_btree_close(session);
	__wt_free(session, btree->name);
	__wt_free(session, btree->filename);
	__wt_free(session, btree->config);

	WT_TRET(__wt_rwlock_destroy(session, btree->rwlock));
	__wt_free(session, session->btree);

	return (ret);
}

/*
 * __wt_conn_reopen_btree --
 *	Reset an open btree handle back to its initial state.
 */
int
__wt_conn_reopen_btree(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_RET(__wt_btree_close(session));
	WT_RET(__wt_btree_open(session, NULL, flags));

	return (0);
}
