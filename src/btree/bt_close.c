/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bt_close --
 *	Close the tree.
 */
int
__wt_bt_close(SESSION *session)
{
	BTREE *btree;
	int ret;

	btree = session->btree;
	ret = 0;

	/* Ask the eviction thread to flush all pages. */
	__wt_evict_file_serial(session, 1, ret);

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

	/* Write out the file's meta-data. */
	WT_TRET(__wt_desc_write(session));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(session, btree->fh));
	btree->fh = NULL;

	return (ret);
}
