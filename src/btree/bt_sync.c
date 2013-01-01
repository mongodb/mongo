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
 * __wt_bt_cache_flush --
 *	Write dirty pages from the cache, optionally discarding the file.
 */
int
__wt_bt_cache_flush(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, int op)
{
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * Flush dirty pages, and optionally discard the file from the cache.
	 *
	 * Reconciliation is just another reader of the page, so with some
	 * care, it can be done in the current thread, leaving the eviction
	 * thread to keep freeing spaces if the cache is full.  Sync and
	 * eviction cannot operate on the same page at the same time, and there
	 * are different modes inside __wt_tree_walk to make sure they don't
	 * trip over each other.
	 *
	 * A further complication is that pages that appear in a checkpoint
	 * cannot be freed until the block lists for the checkpoint are stable.
	 * This is dealt with this by locking out eviction of dirty pages while
	 * writing the internal nodes of a tree.
	 */

	/*
	 * XXX
	 * Set the checkpoint reference for reconciliation -- this is ugly,
	 * but there's no data structure path from here to reconciliation.
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(btree->ckpt, ckptbase);

	/* Ordinary checkpoints are done in the calling thread. */
	if (op == WT_SYNC_INTERNAL || op == WT_SYNC_LEAF)
		ret = __wt_sync_file(session, op);
	else {
		/*
		 * Schedule and wake the eviction server, then wait for the
		 * eviction server to wake us.
		 */
		WT_ERR(__wt_sync_file_serial(session, op));
		WT_ERR(__wt_evict_server_wake(session));
		WT_ERR(__wt_cond_wait(session, session->cond, 0));
		ret = session->syncop_ret;
	}

	switch (op) {
	case WT_SYNC_DISCARD:
	case WT_SYNC_DISCARD_NOWRITE:
		/* If discarding the tree, the root page should be gone. */
		WT_ASSERT(session, ret != 0 || btree->root_page == NULL);
		break;
	}

err:	btree->ckpt = NULL;
	return (ret);
}
