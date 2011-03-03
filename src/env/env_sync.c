/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_sync --
 *	Flush the environment's cache.
 */
int
__wt_connection_sync(CONNECTION *conn, void (*f)(const char *, uint64_t))
{
	BTREE *btree;
	int ret;

	ret = 0;

	TAILQ_FOREACH(btree, &conn->dbqh, q)
		WT_TRET(btree->sync(btree, f, 0));

	return (ret);
}
