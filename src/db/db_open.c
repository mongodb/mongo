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

	WT_STAT_INCR(btree->conn->stats, file_open);

	WT_RET(__wt_strdup(session, name, &btree->name));
	btree->mode = mode;

	__wt_lock(session, conn->mtx);
	btree->file_id = ++conn->next_file_id;
	__wt_unlock(session, conn->mtx);

	/* Open the underlying Btree. */
	WT_RET(__wt_bt_open(session, LF_ISSET(WT_CREATE) ? 1 : 0));

	return (0);
}

/*
 * __wt_btree_close --
 *	Db.close method (BTREE close & handle destructor).
 */
int
__wt_btree_close(SESSION *session)
{
	BTREE *btree;
	CONNECTION *conn;
	int inuse, ret;

	btree = session->btree;
	conn = S2C(session);

	__wt_lock(session, conn->mtx);
	WT_ASSERT(session, btree->refcnt > 0);
	inuse = (--btree->refcnt > 0);
	__wt_unlock(session, conn->mtx);

	if (inuse)
		return (0);

	/* Close the underlying Btree. */
	ret = __wt_bt_close(session);
	WT_TRET(__wt_btree_destroy(btree));

	return (ret);
}
