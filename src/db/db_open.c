/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_open --
 *	Open a BTREE handle.
 */
int
__wt_btree_open(SESSION *session, const char *name, mode_t mode, uint32_t flags)
{
	CONNECTION *conn;
	BTREE *btree;

	btree = session->btree;
	conn = btree->conn;

	WT_STAT_INCR(btree->conn->stats, FILE_OPEN);

	WT_RET(__wt_strdup(session, name, &btree->name));
	btree->mode = mode;

	__wt_lock(session, conn->mtx);
	btree->file_id = ++conn->next_file_id;
	__wt_unlock(session, conn->mtx);

	/*
	 * XXX
	 * Initialize the root location to point to the start of the file.
	 * This is all wrong, and we'll get the information from somewhere
	 * else, eventually.
	 */
	WT_CLEAR(btree->root_page);

	if (LF_ISSET(WT_RDONLY))
		F_SET(btree, WT_RDONLY);

	/* Open the underlying Btree. */
	WT_RET(__wt_bt_open(session, LF_ISSET(WT_CREATE) ? 1 : 0));

	/* Turn on the methods that require open. */
	__wt_methods_btree_open_transition(btree);

	/* Put the BTREE handle into the cache. */
	WT_RET(__wt_session_add_btree(session, btree));

	return (0);
}

/*
 * __wt_btree_close --
 *	Db.close method (BTREE close & handle destructor).
 */
int
__wt_btree_close(SESSION *session, uint32_t flags)
{
	BTREE *btree;
	int ret;

	WT_UNUSED(flags);

	btree = session->btree;
	ret = 0;

	/* Close the underlying Btree. */
	ret = __wt_bt_close(session);

	WT_TRET(__wt_btree_destroy(btree));

	return (ret);
}
