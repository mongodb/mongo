/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_session_add_btree --
 *	Add a btree handle to the session's cache.
 */
int
__wt_session_add_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE_SESSION *btree_session;
	WT_CONNECTION_IMPL *conn;

	WT_RET(__wt_calloc_def(session, 1, &btree_session));
	btree_session->btree = session->btree;
	conn = S2C(session);

	TAILQ_INSERT_HEAD(&session->btrees, btree_session, q);

	__wt_lock(session, conn->mtx);
	++session->btree->refcnt;
	__wt_unlock(session, conn->mtx);

	if (btree_sessionp != NULL)
		*btree_sessionp = btree_session;

	return (0);
}

/*
 * __wt_session_get_btree --
 *	Get the btree handle for the named table.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *name, size_t namelen, WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(name, btree->name, namelen) == 0 &&
		    btree->name[namelen] == '\0') {
			if (btree_sessionp != NULL)
				*btree_sessionp = btree_session;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_session_remove_btree --
 *	Remove the btree handle from the session, closing if necessary.
 */
int
__wt_session_remove_btree(
    WT_SESSION_IMPL *session, WT_BTREE_SESSION *btree_session)
{
	WT_CONNECTION_IMPL *conn;
	int need_close;

	conn = S2C(session);

	TAILQ_REMOVE(&session->btrees, btree_session, q);
	session->btree = btree_session->btree;
	__wt_free(session, btree_session);

	__wt_lock(session, conn->mtx);
	WT_ASSERT(session, session->btree->refcnt > 0);
	need_close = (--session->btree->refcnt == 0);
	__wt_unlock(session, conn->mtx);

	return (need_close ? __wt_btree_close(session) : 0);
}
