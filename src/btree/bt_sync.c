/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bt_sync --
 *	Sync the tree.
 */
int
__wt_bt_sync(SESSION *session)
{
	BTREE *btree;
	int ret;

	btree = session->btree;

	if (WT_UNOPENED_FILE(btree))
		return (0);

	/* Ask the eviction thread to flush any dirty pages. */
	__wt_evict_file_serial(session, btree, 0, ret);

	return (ret);
}
