/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_btree_btree_open(BTREE *, const char *, mode_t, uint32_t);

/*
 * __wt_btree_open --
 *	Open a BTREE handle.
 */
int
__wt_btree_open(SESSION *session, const char *name, mode_t mode, uint32_t flags)
{
	BTREE *btree;

	btree = session->btree;

	WT_STAT_INCR(btree->conn->stats, FILE_OPEN);

	/* Initialize the BTREE structure. */
	WT_RET(__wt_btree_btree_open(btree, name, mode, flags));

	/* Open the underlying Btree. */
	WT_RET(__wt_bt_open(session, LF_ISSET(WT_CREATE) ? 1 : 0));

	/* Turn on the methods that require open. */
	__wt_methods_btree_open_transition(btree);

	return (0);
}

/*
 * __wt_btree_btree_open --
 *	Routine to intialize any BTREE values based on a BTREE value during open.
 */
static int
__wt_btree_btree_open(BTREE *btree, const char *name, mode_t mode, uint32_t flags)
{
	CONNECTION *conn;
	SESSION *session;

	conn = btree->conn;
	session = &conn->default_session;

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

	/* Initialize the zero-length WT_ITEM. */
	WT_ITEM_SET_TYPE(&btree->empty_item, WT_ITEM_DATA);
	WT_ITEM_SET_LEN(&btree->empty_item, 0);

	if (LF_ISSET(WT_RDONLY))
		F_SET(btree, WT_RDONLY);

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

	btree = session->btree;
	ret = 0;

	/* Flush the underlying Btree. */
	if (!LF_ISSET(WT_NOWRITE))
		WT_TRET(__wt_bt_sync(session));

	/* Close the underlying Btree. */
	ret = __wt_bt_close(session);

	WT_TRET(__wt_btree_destroy(btree));

	return (ret);
}
