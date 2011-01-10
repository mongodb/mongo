/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_close_page(WT_TOC *, WT_PAGE *, void *);

/*
 * __wt_bt_close --
 *	Close the tree.
 */
int
__wt_bt_close(WT_TOC *toc)
{
	ENV *env;
	IDB *idb;
	WT_CACHE *cache;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	cache = env->ienv->cache;
	ret = 0;

	/*
	 * XXX
	 * We assume two threads can't call the close method at the same time,
	 * nor can close be called while other threads are in the tree -- the
	 * higher level API has to ensure this.
	 */

	if (WT_UNOPENED_DATABASE(idb))
		return (0);

	/*
	 * The tree walk is depth first, that is, the worker function is not
	 * called on internal pages until all children have been visited; so,
	 * we don't have to worry about a page being dirtied after the visit.
	 *
	 * Lock out the cache drain thread, though, we don't want it trying
	 * to reconcile pages we're flushing.
	 */
	__wt_lock(env, cache->mtx_reconcile);
	WT_TRET(__wt_bt_tree_walk(toc, NULL, 1, __wt_bt_close_page, NULL));
	__wt_unlock(env, cache->mtx_reconcile);

	/* There's no root page any more, kill the pointer to catch mistakes. */
	idb->root_page.page = NULL;

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	return (ret);
}

/*
 * __wt_bt_close_page --
 *	Close a page.
 */
static int
__wt_bt_close_page(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	/* Reconcile any dirty pages, then discard the page. */
	if (WT_PAGE_IS_MODIFIED(page))
		WT_RET(__wt_bt_rec_page(toc, page));

	__wt_bt_page_discard(toc, page);

	return (0);
}
