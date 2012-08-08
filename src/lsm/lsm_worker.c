/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_worker --
 *	The worker thread for an LSM tree, responsible for writing in-memory
 *	trees to disk and merging on-disk trees.
 */
void *
__wt_lsm_worker(void *arg)
{
	WT_CONNECTION *wt_conn;
	WT_CONNECTION_IMPL *conn;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION *session;

	lsm_tree = arg;
	conn = lsm_tree->conn;
	wt_conn = &conn->iface;

	if (wt_conn->open_session(wt_conn, NULL, NULL, &session) != 0)
		return (NULL);

	while (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN))
		__wt_yield();

	return (NULL);
}
