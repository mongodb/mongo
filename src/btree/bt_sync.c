/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_btree_sync --
 *	Sync the tree.
 */
int
__wt_btree_sync(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	return (__wt_btree_snapshot(session, NULL, 0));
}

/*
 * __wt_btree_snapshot --
 *	Snapshot the tree.
 */
int
__wt_btree_snapshot(WT_SESSION_IMPL *session, const char *name, int discard)
{
	WT_BTREE *btree;
	int ret;

	WT_UNUSED(name);

	btree = session->btree;
	ret = 0;

	/* Allocate a temporary buffer for the snapshot information. */
	WT_RET(__wt_scr_alloc(session, WT_BM_MAX_ADDR_COOKIE, &btree->snap));

	/* Snapshots are single-threaded. */
	__wt_readlock(session, btree->snaplock);

	/* Get the current snapshot information. */
	WT_ERR(__wt_btree_get_root(session, btree->snap));

	/*
	 * Ask the eviction thread to flush any dirty pages.
	 *
	 * Reconciliation is just another reader of the page, so it's probably
	 * possible to do this work in the current thread, rather than poking
	 * the eviction thread.  The trick is for the sync thread to acquire
	 * hazard references on each dirty page, which would block the eviction
	 * thread from attempting to evict the pages.
	 *
	 * I'm not doing it for a few reasons: (1) I don't want sync to update
	 * the page's read-generation number, and eviction already knows how to
	 * do that, (2) I don't want more than a single sync running at a time,
	 * and calling eviction makes it work that way without additional work,
	 * (3) sync and eviction cannot operate on the same page at the same
	 * time and I'd rather they didn't operate in the same file at the same
	 * time, and using eviction makes it work that way without additional
	 * synchronization, (4) eventually we'll have async write I/O, and this
	 * approach results in one less page writer in the system, (5) the code
	 * already works that way.   None of these problems can't be fixed, but
	 * I don't see a reason to change at this time, either.
	 */
	do {
		ret = __wt_sync_file_serial(session, discard);
	} while (ret == WT_RESTART);
	WT_ERR(ret);

	/* If we're discarding the tree, the root page should be gone. */
	WT_ASSERT(session, !discard || btree->root_page == NULL);

	/* After all pages are evicted, update the snapshot information. */
	if (btree->snap->data != NULL && btree->snap->size != 0)
		WT_ERR(__wt_btree_set_root(session, btree->filename,
		    (uint8_t *)btree->snap->data, btree->snap->size));

err:	__wt_rwunlock(session, btree->snaplock);
	__wt_scr_free(&btree->snap);
	btree->snap = NULL;

	return (ret);
}
