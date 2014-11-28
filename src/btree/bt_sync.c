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
	WT_PAGE_MODIFY *mod;
	WT_REF *walk;
	WT_TXN *txn;
	uint64_t internal_bytes, leaf_bytes;
	uint64_t internal_pages, leaf_pages;
	uint32_t flags;

	btree = S2BT(session);

	flags = WT_READ_CACHE | WT_READ_NO_GEN;
	walk = NULL;
	txn = &session->txn;

	internal_bytes = leaf_bytes = 0;
	internal_pages = leaf_pages = 0;
	if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
		WT_RET(__wt_epoch(session, &start));

	switch (syncop) {
	case WT_SYNC_WRITE_LEAVES:
		/*
		 * Write all immediately available, dirty in-cache leaf pages.
		 *
		 * Writing the leaf pages is done without acquiring a high-level
		 * lock, serialize so multiple threads don't walk the tree at
		 * the same time.
		 */
		if (!btree->modified)
			return (0);
		__wt_spin_lock(session, &btree->flush_lock);
		if (!btree->modified) {
			__wt_spin_unlock(session, &btree->flush_lock);
			return (0);
		}

		flags |= WT_READ_NO_WAIT | WT_READ_SKIP_INTL;
		for (walk = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk, flags));
			if (walk == NULL)
				break;

			/* Write dirty pages if nobody beat us to it. */
			page = walk->page;
			if (__wt_page_is_modified(page)) {
				if (txn->isolation == TXN_ISO_READ_COMMITTED)
					__wt_txn_refresh(session, 1);
				leaf_bytes += page->memory_footprint;
				++leaf_pages;
				WT_ERR(__wt_reconcile(session, walk, NULL, 0));
			}
		}
		break;
	case WT_SYNC_CHECKPOINT:
		/*
		 * We cannot check the tree modified flag in the case of a
		 * checkpoint, the checkpoint code has already cleared it.
		 *
		 * Writing the leaf pages is done without acquiring a high-level
		 * lock, serialize so multiple threads don't walk the tree at
		 * the same time.  We're holding the schema lock, but need the
		 * lower-level lock as well.
		 */
		__wt_spin_lock(session, &btree->flush_lock);

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

		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
			WT_ERR(__wt_evict_file_exclusive_on(session));
			__wt_evict_file_exclusive_off(session);
		}

		/* Write all dirty in-cache pages. */
		flags |= WT_READ_NO_EVICT;
		for (walk = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk, flags));
			if (walk == NULL)
				break;

			/*
			 * Write dirty pages, unless we can be sure they only
			 * became dirty after the checkpoint started.
			 *
			 * We can skip dirty pages if:
			 * (1) they are leaf pages;
			 * (2) there is a snapshot transaction active (which
			 *     is the case in ordinary application checkpoints
			 *     but not all internal cases); and
			 * (3) the first dirty update on the page is
			 *     sufficiently recent that the checkpoint
			 *     transaction would skip them.
			 */
			page = walk->page;
			mod = page->modify;
			if (__wt_page_is_modified(page) &&
			    (WT_PAGE_IS_INTERNAL(page) ||
			    !F_ISSET(txn, TXN_HAS_SNAPSHOT) ||
			    TXNID_LE(mod->first_dirty_txn, txn->snap_max))) {
				if (WT_PAGE_IS_INTERNAL(page)) {
					internal_bytes +=
					    page->memory_footprint;
					++internal_pages;
				} else {
					leaf_bytes += page->memory_footprint;
					++leaf_pages;
				}
				WT_ERR(__wt_reconcile(session, walk, NULL, 0));
			}
		}
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT)) {
		WT_ERR(__wt_epoch(session, &end));
		WT_ERR(__wt_verbose(session, WT_VERB_CHECKPOINT,
		    "__sync_file WT_SYNC_%s wrote:\n\t %" PRIu64
		    " bytes, %" PRIu64 " pages of leaves\n\t %" PRIu64
		    " bytes, %" PRIu64 " pages of internal\n\t"
		    "Took: %" PRIu64 "ms",
		    syncop == WT_SYNC_WRITE_LEAVES ?
		    "WRITE_LEAVES" : "CHECKPOINT",
		    leaf_bytes, leaf_pages, internal_bytes, internal_pages,
		    WT_TIMEDIFF(end, start) / WT_MILLION));
	}

err:	/* On error, clear any left-over tree walk. */
	if (walk != NULL)
		WT_TRET(__wt_page_release(session, walk, flags));

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

	__wt_spin_unlock(session, &btree->flush_lock);

	/*
	 * Leaves are written before a checkpoint (or as part of a file close,
	 * before checkpointing the file).  Start a flush to stable storage,
	 * but don't wait for it.
	 */
	if (ret == 0 && syncop == WT_SYNC_WRITE_LEAVES)
		WT_RET(btree->bm->sync(btree->bm, session, 1));

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
	    session, &next_ref, WT_READ_CACHE | WT_READ_NO_EVICT));
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
		 *
		 * Eviction can also fail because an update cannot be written.
		 * If sessions have disjoint sets of files open, updates in a
		 * no-longer-referenced file may not yet be globally visible,
		 * and the write will fail with EBUSY.  Our caller handles that
		 * error, retrying later.
		 */
		if (syncop == WT_SYNC_CLOSE && __wt_page_is_modified(page))
			WT_ERR(__wt_reconcile(session, ref, NULL, WT_EVICTING));

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
		    session, &next_ref, WT_READ_CACHE | WT_READ_NO_EVICT));

		switch (syncop) {
		case WT_SYNC_CLOSE:
			/*
			 * Evict the page.
			 * Do not attempt to evict pages expected to be merged
			 * into their parents, with the exception that the root
			 * page can't be merged, it must be written.
			 */
			if (__wt_ref_is_root(ref) ||
			    page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_EMPTY))
				WT_ERR(__wt_evict(session, ref, 1));
			break;
		case WT_SYNC_DISCARD:
		case WT_SYNC_DISCARD_FORCE:
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
			/*
			 * If the page contains an update that is too recent to
			 * evict, stop.  This should never happen during
			 * connection close, and in other paths our caller
			 * should be prepared to deal with this case.
			 */
			if (syncop == WT_SYNC_DISCARD &&
			    page->modify != NULL &&
			    !__wt_txn_visible_all(session,
			    page->modify->rec_max_txn))
				WT_ERR(EBUSY);

			if (syncop == WT_SYNC_DISCARD_FORCE)
				F_SET(session, WT_SESSION_DISCARD_FORCE);
			__wt_rec_page_clean_update(session, ref);
			F_CLR(session, WT_SESSION_DISCARD_FORCE);
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_ref != NULL)
			WT_TRET(__wt_page_release(
			    session, next_ref, WT_READ_NO_EVICT));
	}

	if (eviction_enabled)
		__wt_evict_file_exclusive_off(session);

	return (ret);
}

/*
 * __wt_cache_op --
 *	Cache operations.
 */
int
__wt_cache_op(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, int op)
{
	WT_DECL_RET;
	WT_BTREE *btree;

	btree = S2BT(session);

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_CLOSE:
		/*
		 * Set the checkpoint reference for reconciliation; it's ugly,
		 * but drilling a function parameter path from our callers to
		 * the reconciliation of the tree's root page is going to be
		 * worse.
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
	case WT_SYNC_CLOSE:
	case WT_SYNC_DISCARD:
	case WT_SYNC_DISCARD_FORCE:
		WT_ERR(__evict_file(session, op));
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_CLOSE:
		btree->ckpt = NULL;
		break;
	}

	return (ret);
}
