/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
			WT_ERR(__wt_tree_walk(session, &walk, NULL, flags));
			if (walk == NULL)
				break;

			/*
			 * Write dirty pages if nobody beat us to it.  Don't
			 * try to write the hottest pages: checkpoint will have
			 * to visit them anyway.
			 */
			page = walk->page;
			if (__wt_page_is_modified(page) &&
			    __wt_txn_visible_all(
			    session, page->modify->update_txn)) {
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
			WT_ERR(__wt_tree_walk(session, &walk, NULL, flags));
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
		 * If this tree was being skipped by the eviction server during
		 * the checkpoint, clear the wait.
		 */
		btree->evict_walk_period = 0;

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
		WT_ERR(__wt_evict_file(session, op));
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
