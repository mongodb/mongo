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
		return (0);

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

	/* There's no root page any more, kill the pointer to catch mistakes. */
	btree->root_page.page = NULL;

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(session, btree->fh));
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
	 * Reconcile any dirty pages, then discard the page.
	 *
	 * The tree walk is depth first, that is, the worker function is not
	 * called on internal pages until all children have been visited; so,
	 * we don't have to worry about reconciling a page that still has a
	 * child page, or reading a page after we discard it,
	 *
	 * We ignore WT_REF connections because file close is single-threaded
	 * by the API layer, there's no other threads of control in the system.
	 */
	if (WT_PAGE_IS_MODIFIED(page))
		WT_RET(__wt_page_reconcile(session, page));

	__wt_page_discard(session, page);
	return (0);
}
