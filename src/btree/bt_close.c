/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_bt_close_page(SESSION *, WT_PAGE *, void *);

/*
 * __wt_bt_close --
 *	Close the tree.
 */
int
__wt_bt_close(SESSION *session)
{
	BTREE *btree;
	WT_CACHE *cache;
	int ret;

	btree = session->btree;
	cache = S2C(session)->cache;
	ret = 0;

	/*
	 * XXX
	 * We assume two threads can't call the close method at the same time,
	 * nor can close be called while other threads are in the tree -- the
	 * higher level API has to ensure this.
	 */
	if (WT_UNOPENED_FILE(btree))
		goto done;

	/*
	 * The tree walk is depth first, that is, the worker function is not
	 * called on internal pages until all children have been visited; so,
	 * we don't have to worry about a page being dirtied after the visit.
	 *
	 * Lock out the cache evictions thread, though, we don't want it trying
	 * to evict pages we're flushing.
	 */
	__wt_lock(session, cache->mtx_reconcile);
	WT_TRET(__wt_tree_walk(
	    session, NULL, WT_WALK_CACHE, __wt_bt_close_page, NULL));
	__wt_evict_db_clear(session);
	__wt_unlock(session, cache->mtx_reconcile);

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

	/* Close the underlying file handle. */
done:	WT_TRET(__wt_close(session, btree->fh));
	btree->fh = NULL;

	return (ret);
}

/*
 * __wt_bt_close_page --
 *	Close a page.
 */
static int
__wt_bt_close_page(SESSION *session, WT_PAGE *page, void *arg)
{
	WT_UNUSED(arg);

	/*
	 * We ignore hazard references because close is single-threaded by the
	 * API layer, there's no other threads of control in the tree.
	 *
	 * We're discarding the page; call the reconciliation code to update
	 * the parent's information.  It's probably possible to just discard
	 * the page, but I'd rather keep the tree in a consistent state even
	 * while discarding it.
	 *
	 * The tree walk is depth first, that is, the worker function is not
	 * called on internal pages until all children have been visited; we
	 * don't have to worry about reconciling a page that still has a child
	 * page, or reading a page after we discard it,
	 */
	return (__wt_page_reconcile(session, page, 0, 1));
}
