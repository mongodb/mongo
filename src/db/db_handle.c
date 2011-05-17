/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_session_btree --
 *	BTREE constructor.
 */
int
__wt_session_btree(SESSION *session)
{
	BTREE *btree;
	CONNECTION *conn;

	conn = S2C(session);

	/* Create the BTREE structure. */
	WT_RET(__wt_calloc_def(session, 1, &btree));

	/* Connect everything together. */
	btree->conn = conn;

	/* Add to the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->dbqh, btree, q);
	++conn->dbqcnt;
	__wt_unlock(session, conn->mtx);

	session->btree = btree;
	return (0);
}
