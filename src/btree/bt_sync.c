/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bt_cache_force_write --
 *	Dirty the root page of the tree so it gets written.
 */
int
__wt_bt_cache_force_write(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = session->btree;
	page = btree->root_page;

	/* Dirty the root page to ensure a write. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	return (0);
}

/*
 * __wt_bt_cache_op --
 *	Cache operations: compaction, discard, sync/checkpoint.
 */
int
__wt_bt_cache_op(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, int op)
{
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * Compaction and sync/checkpoint reconcile dirty pages from the cache
	 * to the backing block manager.  Reconciliation is just another reader
	 * of the page, so with some care, it can be done in the current thread,
	 * leaving the eviction thread to keep freeing spaces if the cache is
	 * full.  Sync and eviction cannot operate on the same page at the same
	 * time, and there are different modes inside __wt_tree_walk to make
	 * sure they don't trip over each other.
	 *
	 * The current thread cannot evict pages from the cache, so discard is
	 * done by calling the eviction server for service.
	 *
	 * XXX
	 * Set the checkpoint reference for reconciliation -- this is ugly, but
	 * there's no data structure path from here to reconciliation.
	 *
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(btree->ckpt, ckptbase);

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_COMPACT:
	case WT_SYNC_WRITE_LEAVES:
		WT_ERR(__wt_sync_file(session, op));
		break;
	case WT_SYNC_DISCARD:
	case WT_SYNC_DISCARD_NOWRITE:
		/*
		 * Schedule and wake the eviction server, then wait for the
		 * eviction server to wake us.
		 */
		WT_ERR(__wt_sync_file_serial(session, op));
		WT_ERR(__wt_evict_server_wake(session));
		WT_ERR(__wt_cond_wait(session, session->cond, 0));
		ret = session->syncop_ret;

		/* If discarding the tree, the root page should be gone. */
		WT_ASSERT(session, ret != 0 || btree->root_page == NULL);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	btree->ckpt = NULL;
	return (ret);
}
