/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __sync_file --
 *	Flush pages for a specific file.
 */
static int
__sync_file(WT_SESSION_IMPL *session, int syncop)
{
	struct timespec end, start;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *walk_page;
	WT_TXN *txn;
	uint64_t internal_bytes, leaf_bytes;
	uint64_t internal_pages, leaf_pages;
	uint32_t checkpoint_gen, flags;

	btree = S2BT(session);
	walk_page = NULL;
	txn = &session->txn;

	internal_bytes = leaf_bytes = 0;
	internal_pages = leaf_pages = 0;
	if (WT_VERBOSE_ISSET(session, checkpoint))
		WT_ERR(__wt_epoch(session, &start));

	switch (syncop) {
	case WT_SYNC_WRITE_LEAVES:
		/*
		 * Write all immediately available, dirty in-cache leaf pages.
		 */
		flags = WT_READ_CACHE |
		    WT_READ_NO_GEN | WT_READ_NO_WAIT | WT_READ_SKIP_INTL;
		for (walk_page = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk_page, flags));
			if (walk_page == NULL)
				break;

			/* Write dirty pages if nobody beat us to it. */
			page = walk_page->page;
			if (__wt_page_is_modified(page)) {
				if (txn->isolation == TXN_ISO_READ_COMMITTED)
					__wt_txn_refresh(
					    session, WT_TXN_NONE, 1);
				leaf_bytes += page->memory_footprint;
				++leaf_pages;
				WT_ERR(__wt_rec_write(
				    session, walk_page, NULL, 0));
			}
		}
		break;

	case WT_SYNC_CHECKPOINT:
		/*
		 * When internal pages are being reconciled by checkpoint their
		 * child pages cannot disappear from underneath them or be split
		 * into them, nor can underlying blocks be freed until the block
		 * lists for the checkpoint are stable.  Set the checkpointing
		 * flag to block eviction of dirty pages until the checkpoint's
		 * internal page pass is complete, then wait for any existing
		 * eviction to complete.
		 */
		btree->checkpointing = 1;
		checkpoint_gen = S2C(session)->txn_global.checkpoint_gen;

		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
			WT_ERR(__wt_evict_file_exclusive_on(session));
			__wt_evict_file_exclusive_off(session);
		}

		/*
		 * Write all dirty in-cache pages.
		 */
		flags = WT_READ_CACHE | WT_READ_NO_GEN;
		for (walk_page = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk_page, flags));
			if (walk_page == NULL)
				break;

			/*
			 * Write dirty pages, unless we can be sure they only
			 * became dirty after the checkpoint started.
			 *
			 * XXX this test is not precise -- there is a race when
			 * the checkpoint is starting, and between concurrent
			 * transactions updating the same page.  It is intended
			 * for performance evaluation only
			 */
			page = walk_page->page;
			if (__wt_page_is_modified(page) &&
			    (WT_PAGE_IS_INTERNAL(page) ||
			    page->modify->checkpoint_gen == 0 ||
			    page->modify->checkpoint_gen < checkpoint_gen ||
			    TXNID_LE(page->modify->rec_min_skipped_txn,
			    session->txn.snap_max))) {
				internal_bytes += page->memory_footprint;
				++internal_pages;
				WT_ERR(__wt_rec_write(
				    session, walk_page, NULL, 0));
			}

			/*
			 * Set the checkpoint generation, even if we didn't
			 * write the page.  If it becomes dirty and is selected
			 * for eviction, it can't be written until this
			 * checkpoint completes.
			 */
			if (page->modify != NULL)
				page->modify->checkpoint_gen = checkpoint_gen;
		}
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	if (WT_VERBOSE_ISSET(session, checkpoint)) {
		WT_ERR(__wt_epoch(session, &end));
		WT_VERBOSE_ERR(session, checkpoint,
		    "__sync_file WT_SYNC_%s wrote:\n\t %" PRIu64
		    " bytes, %" PRIu64 " pages of leaves\n\t %" PRIu64
		    " bytes, %" PRIu64 " pages of internal\n\t"
		    "Took: %" PRIu64 "ms",
		    syncop == WT_SYNC_WRITE_LEAVES ?
		    "WRITE_LEAVES" : "CHECKPOINT",
		    leaf_bytes, leaf_pages, internal_bytes, internal_pages,
		    WT_TIMEDIFF(end, start) / WT_MILLION);
	}

err:	/* On error, clear any left-over tree walk. */
	if (walk_page != NULL)
		WT_TRET(__wt_page_release(session, walk_page));

	if (txn->isolation == TXN_ISO_READ_COMMITTED && session->ncursors == 0)
		__wt_txn_release_snapshot(session);

	if (btree->checkpointing) {
		/*
		 * Clear the checkpoint flag and push the change; not required,
		 * but publishing the change means stalled eviction gets moving
		 * as soon as possible.
		 */
		btree->checkpointing = 0;
		WT_FULL_BARRIER();

		/*
		 * Wake the eviction server, in case application threads have
		 * stalled while the eviction server decided it couldn't make
		 * progress.  Without this, application threads will be stalled
		 * until the eviction server next wakes.
		 */
		WT_TRET(__wt_evict_server_wake(session));
	}

	return (ret);
}

/*
 * __evict_file --
 *	Discard pages for a specific file.
 */
static int
__evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *next_ref, *ref;
	int eviction_enabled;

	btree = S2BT(session);
	eviction_enabled = !F_ISSET(btree, WT_BTREE_NO_EVICTION);

	/*
	 * We need exclusive access to the file -- disable ordinary eviction
	 * and drain any blocks already queued.
	 */
	if (eviction_enabled)
		WT_RET(__wt_evict_file_exclusive_on(session));

	/* Make sure the oldest transaction ID is up-to-date. */
	__wt_txn_update_oldest(session);

	/* Walk the tree, discarding pages. */
	next_ref = NULL;
	WT_ERR(__wt_tree_walk(
	    session, &next_ref, WT_READ_CACHE | WT_READ_NO_GEN));
	while ((ref = next_ref) != NULL) {
		page = ref->page;

		/*
		 * Eviction can fail when a page in the evicted page's subtree
		 * switches state.  For example, if we don't evict a page marked
		 * empty, because we expect it to be merged into its parent, it
		 * might no longer be empty after it's reconciled, in which case
		 * eviction of its parent would fail.  We can either walk the
		 * tree multiple times (until it's finally empty), or reconcile
		 * each page to get it to its final state before considering if
		 * it's an eviction target or will be merged into its parent.
		 *
		 * Don't limit this test to any particular page type, that tends
		 * to introduce bugs when the reconciliation of other page types
		 * changes, and there's no advantage to doing so.
		 */
		if (syncop == WT_SYNC_DISCARD && __wt_page_is_modified(page))
			WT_ERR(__wt_rec_write(
			    session, ref, NULL, WT_SKIP_UPDATE_ERR));

		/*
		 * We can't evict the page just returned to us (it marks our
		 * place in the tree), so move the walk to one page ahead of
		 * the page being evicted.  Note, we reconciled the returned
		 * page first: if reconciliation of that page were to change
		 * the shape of the tree, and we did the next walk call before
		 * the reconciliation, the next walk call could miss a page in
		 * the tree.
		 */
		WT_ERR(__wt_tree_walk(
		    session, &next_ref, WT_READ_CACHE | WT_READ_NO_GEN));

		switch (syncop) {
		case WT_SYNC_DISCARD:
			/*
			 * Evict the page.
			 * Do not attempt to evict pages expected to be merged
			 * into their parents, with the exception that the root
			 * page can't be merged, it must be written.
			 */
			if (__wt_ref_is_root(ref) ||
			    page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_EMPTY))
				WT_ERR(__wt_rec_evict(session, ref, 1));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			/*
			 * Discard the page, whether clean or dirty.
			 *
			 * Clean the page, both to keep statistics correct, and
			 * to let the page-discard function assert no dirty page
			 * is ever discarded.
			 */
			if (__wt_page_is_modified(page)) {
				page->modify->write_gen = 0;
				__wt_cache_dirty_decr(session, page);
			}
			__wt_ref_out(session, ref);
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_ref != NULL)
			WT_TRET(__wt_page_release(session, next_ref));
	}

	if (eviction_enabled)
		__wt_evict_file_exclusive_off(session);

	return (ret);
}

/*
 * __wt_bt_cache_force_write --
 *	Dirty the root page of the tree so it gets written.
 */
int
__wt_bt_cache_force_write(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = S2BT(session);
	page = btree->root.page;

	/* Dirty the root page to ensure a write. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	return (0);
}

/*
 * __wt_bt_cache_op --
 *	Cache operations.
 */
int
__wt_bt_cache_op(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, int op)
{
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = S2BT(session);

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_DISCARD:
		/*
		 * XXX
		 * Set the checkpoint reference for reconciliation -- this is
		 * ugly, but there's no data structure path from here to the
		 * reconciliation of the tree's root page.
		 */
		WT_ASSERT(session, btree->ckpt == NULL);
		btree->ckpt = ckptbase;
		break;
	}

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_WRITE_LEAVES:
		WT_ERR(__sync_file(session, op));
		break;
	case WT_SYNC_DISCARD:
	case WT_SYNC_DISCARD_NOWRITE:
		WT_ERR(__evict_file(session, op));
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_DISCARD:
		btree->ckpt = NULL;
		break;
	}

	return (ret);
}
