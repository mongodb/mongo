/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_bt_tree_sync(SESSION *, WT_PAGE *, void *);

/*
 * __wt_bt_sync --
 *	Sync the tree.
 */
int
__wt_bt_sync(SESSION *session)
{
	BTREE *btree;
	WT_CACHE *cache;
	int ret;

	btree = session->btree;
	cache = S2C(session)->cache;

	if (WT_UNOPENED_FILE(btree))
		return (0);

	/*
	 * The tree walk is depth first, that is, the worker function is not
	 * called on internal pages until all children have been visited; so,
	 * we don't have to worry about a page being dirtied after the visit.
	 *
	 * Lock out the cache eviction thread, though, we don't want it trying
	 * to reconcile pages we're flushing.
	 */
	__wt_lock(session, cache->mtx_reconcile);
	ret = __wt_tree_walk(session, NULL, WT_WALK_CACHE, __wt_bt_tree_sync, NULL);
	__wt_unlock(session, cache->mtx_reconcile);
	return (ret);
}

/*
 * __wt_bt_tree_sync --
 *	Sync a page.
 */
static int
__wt_bt_tree_sync(SESSION *session, WT_PAGE *page, void *arg)
{
	WT_UNUSED(arg);

	/* Reconcile any dirty pages. */
	if (WT_PAGE_IS_MODIFIED(page))
		WT_RET(__wt_page_reconcile(session, page));
	return (0);
}
