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

	if (btree_sessionp != NULL)
		*btree_sessionp = btree_session;

	return (0);
}

/*
 * __wt_session_find_btree --
 *	Find an open btree handle for the named table.
 */
int
__wt_session_find_btree(WT_SESSION_IMPL *session,
    const char *filename, size_t namelen, WT_BTREE_SESSION **btree_sessionp)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;

	TAILQ_FOREACH(btree_session, &session->btrees, q) {
		btree = btree_session->btree;
		if (strncmp(filename, btree->filename, namelen) == 0 &&
		    btree->filename[namelen] == '\0') {
			if (btree_sessionp != NULL)
				*btree_sessionp = btree_session;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_session_get_btree --
 *	Get a btree handle for the given name, set session->btree.
 */
int
__wt_session_get_btree(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE_SESSION *btree_session;
	const char *filename, *treeconf;
	int ret;

	filename = name;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (EINVAL);

	if ((ret = __wt_session_find_btree(session,
	    filename, strlen(filename), &btree_session)) == 0)
		session->btree = btree_session->btree;
	else if (ret == WT_NOTFOUND) {
		WT_RET(__wt_schema_table_read(session, name, &treeconf));
		WT_RET(__wt_btree_open(session, name, filename, treeconf, 0));
		WT_RET(__wt_session_add_btree(session, &btree_session));
		ret = 0;
	}

	return (ret);
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

	conn = S2C(session);

	TAILQ_REMOVE(&session->btrees, btree_session, q);
	session->btree = btree_session->btree;
	__wt_free(session, btree_session);

	return (__wt_btree_close(session));
}
