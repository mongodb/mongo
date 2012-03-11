/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_btree_open --
 *	Find an open btree file handle, otherwise create a new one and link it
 * into the connection's list.
 */
int
__wt_conn_btree_open(WT_SESSION_IMPL *session,
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
		matched = F_ISSET(btree, WT_BTREE_OPEN) ? 1 : 0;
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
	if (ret == 0)
		F_SET(btree, WT_BTREE_OPEN);
	__wt_rwunlock(session, btree->rwlock);
	if (ret == 0)
		return (0);

	(void)__wt_conn_btree_close(session);
	return (ret);
}

/*
 * __wt_conn_btree_close --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_btree_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	int ret;

	btree = session->btree;
	conn = S2C(session);
	ret = 0;

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	/*
	 * If it looks like we are the last reference, sync the file.
	 * This should mean the close call made while holding the spinlock is
	 * fast.
	 */
	if (btree->refcnt == 1)
		WT_RET(__wt_btree_sync(session, NULL));

	/* Decrement the reference count. */
	__wt_spin_lock(session, &conn->spinlock);
	if (--btree->refcnt == 0) {
		ret = __wt_btree_close(session);
		F_CLR(btree, WT_BTREE_OPEN);
	}
	__wt_spin_unlock(session, &conn->spinlock);

	return (ret);
}

/*
 * __conn_btree_remove --
 *	Discard a single btree file handle structure.
 */
static int
__conn_btree_remove(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	int ret;

	ret = 0;

	WT_SET_BTREE_IN_SESSION(session, btree);
	WT_TRET(__wt_btree_close(session));
	WT_TRET(__wt_rwlock_destroy(session, btree->rwlock));
	__wt_free(session, btree->filename);
	__wt_free(session, btree->name);
	__wt_free(session, btree->config);
	__wt_free(session, btree);
	WT_CLEAR_BTREE_IN_SESSION(session);

	return (ret);
}

/*
 * __wt_conn_btree_remove --
 *	Discard the btree file handle structures.
 */
int
__wt_conn_btree_remove(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;
	WT_SESSION_IMPL *session;
	int ret;

	ret = 0;

	/*
	 * We need a session handle because we're potentially reading/writing
	 * pages.
	 */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &session));

	/*
	 * Close open btree handles: first, everything but the schema file (as
	 * closing a normal file may open and write the schema file), then the
	 * schema file.  This function isn't called often, and I don't want to
	 * "know" anything about the schema file's position on the list, so we
	 * do it the hard way.
	 */
restart:
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0)
			continue;

		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
		WT_TRET(__conn_btree_remove(session, btree));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our session's list
	 * of open btree handles, specifically, we added the schema file if any
	 * of the files were dirty.  Clean up that list before we shut down the
	 * schema file entry, for good.
	 */
	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL)
		WT_TRET(__wt_session_remove_btree(session, btree_session));

	/* Close the schema file handle. */
	while ((btree = TAILQ_FIRST(&conn->btqh)) != NULL) {
		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
		WT_TRET(__conn_btree_remove(session, btree));
	}

	/* Discard our session. */
	WT_TRET(session->iface.close(&session->iface, NULL));

	return (ret);
}

/*
 * __wt_conn_btree_reopen --
 *	Reset an open btree handle back to its initial state.
 */
int
__wt_conn_btree_reopen(
    WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_RET(__wt_btree_close(session));
	F_CLR(btree, WT_BTREE_OPEN);
	WT_RET(__wt_btree_open(session, cfg, flags));
	F_SET(btree, WT_BTREE_OPEN);

	return (0);
}
