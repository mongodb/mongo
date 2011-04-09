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

	/*
	 * XXX
	 * We assume two threads can't call the close method at the same time,
	 * nor can close be called while other threads are in the tree -- the
	 * higher level API has to ensure this.
	 */
	if (WT_UNOPENED_FILE(btree))
		goto done;

	/* Ask the eviction thread to flush all pages. */
	__wt_evict_file_serial(session, btree, 1, ret);

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

	/* Close the underlying file handle. */
done:	WT_TRET(__wt_close(session, btree->fh));
	btree->fh = NULL;

	return (ret);
}
