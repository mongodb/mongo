/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bt_cache_flush --
 *	Write dirty pages from the cache, optionally discarding the file.
 */
int
__wt_bt_cache_flush(
    WT_SESSION_IMPL *session, WT_SNAPSHOT *snapbase, int op, int force)
{
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * If we need a new snapshot, mark the root page dirty to ensure a
	 * write.
	 */
	if (force) {
		WT_RET(__wt_page_modify_init(session, btree->root_page));
		__wt_page_modify_set(btree->root_page);
	}

	/*
	 * Ask the eviction thread to flush any dirty pages, and optionally
	 * discard the file from the cache.
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
	btree->snap = snapbase;
	ret = __wt_sync_file_serial(session, op);
	btree->snap = NULL;
	WT_RET(ret);

	switch (op) {
	case WT_SYNC:
		break;
	case WT_SYNC_DISCARD:
		/* If discarding the tree, the root page should be gone. */
		WT_ASSERT(session, btree->root_page == NULL);
		break;
	case WT_SYNC_DISCARD_NOWRITE:
		/*
		 * XXX
		 * I'm not sure this is the right place to do this, but it's
		 * the point in the btree engine where we know the root page
		 * is gone.  Unlike WT_SYNC_DISCARD, which writes, evicts and
		 * discards the root page, WT_SYNC_DISCARD_NOWRITE simply
		 * discards the pages, which means "eviction" never happens.
		 */
		btree->root_page = NULL;
		break;
	}

	return (0);
}
