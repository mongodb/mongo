/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
__sync_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
{
	struct timespec end, start;
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF *walk;
	WT_TXN *txn;
	uint64_t internal_bytes, internal_pages, leaf_bytes, leaf_pages;
	uint64_t oldest_id, saved_snap_min;
	uint32_t flags;

	conn = S2C(session);
	btree = S2BT(session);
	walk = NULL;
	txn = &session->txn;
	saved_snap_min = WT_SESSION_TXN_STATE(session)->snap_min;
	flags = WT_READ_CACHE | WT_READ_NO_GEN;

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

		/*
		 * Save the oldest transaction ID we need to keep around.
		 * Otherwise, in a busy system, we could be updating pages so
		 * fast that write leaves never catches up.  We deliberately
		 * have no transaction running at this point that would keep
		 * the oldest ID from moving forwards as we walk the tree.
		 */
		oldest_id = __wt_txn_oldest_id(session);

		flags |= WT_READ_NO_WAIT | WT_READ_SKIP_INTL;
		for (walk = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk, flags));
			if (walk == NULL)
				break;

			/*
			 * Write dirty pages if nobody beat us to it.  Don't
			 * try to write hot pages (defined as pages that have
			 * been updated since the write phase leaves started):
			 * checkpoint will have to visit them anyway.
			 */
			page = walk->page;
			if (__wt_page_is_modified(page) &&
			    WT_TXNID_LT(page->modify->update_txn, oldest_id)) {
				if (txn->isolation == WT_ISO_READ_COMMITTED)
					WT_ERR(__wt_txn_get_snapshot(session));
				leaf_bytes += page->memory_footprint;
				++leaf_pages;
				WT_ERR(__wt_reconcile(session, walk, NULL, 0));
			}
		}
		break;
	case WT_SYNC_CHECKPOINT:
		/*
		 * If we are flushing a file at read-committed isolation, which
		 * is of particular interest for flushing the metadata to make
		 * schema-changing operation durable, get a transactional
		 * snapshot now.
		 *
		 * All changes committed up to this point should be included.
		 * We don't update the snapshot in between pages because the
		 * metadata shouldn't have many pages.  Instead, read-committed
		 * isolation ensures that all metadata updates completed before
		 * the checkpoint are included.
		 */
		if (txn->isolation == WT_ISO_READ_COMMITTED)
			WT_ERR(__wt_txn_get_snapshot(session));

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
		 * In the final checkpoint pass, child pages cannot be evicted
		 * from underneath internal pages nor can underlying blocks be
		 * freed until the checkpoint's block lists are stable. Also,
		 * we cannot split child pages into parents unless we know the
		 * final pass will write a consistent view of that namespace.
		 * Set the checkpointing flag to block such actions and wait for
		 * any problematic eviction or page splits to complete.
		 */
		WT_PUBLISH(btree->checkpointing, WT_CKPT_PREPARE);

		WT_ERR(__wt_evict_file_exclusive_on(session));
		__wt_evict_file_exclusive_off(session);

		WT_PUBLISH(btree->checkpointing, WT_CKPT_RUNNING);

		/* Write all dirty in-cache pages. */
		flags |= WT_READ_NO_EVICT;
		for (walk = NULL;;) {
			WT_ERR(__wt_tree_walk(session, &walk, flags));
			if (walk == NULL)
				break;

			/* Skip clean pages. */
			if (!__wt_page_is_modified(walk->page))
				continue;

			/*
			 * Take a local reference to the page modify structure
			 * now that we know the page is dirty. It needs to be
			 * done in this order otherwise the page modify
			 * structure could have been created between taking the
			 * reference and checking modified.
			 */
			page = walk->page;
			mod = page->modify;

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
			 *
			 * Mark the tree dirty: the checkpoint marked it clean
			 * and we can't skip future checkpoints until this page
			 * is written.
			 */
			if (!WT_PAGE_IS_INTERNAL(page) &&
			    F_ISSET(txn, WT_TXN_HAS_SNAPSHOT) &&
			    WT_TXNID_LT(txn->snap_max, mod->first_dirty_txn)) {
				__wt_page_modify_set(session, page);
				continue;
			}

			if (WT_PAGE_IS_INTERNAL(page)) {
				internal_bytes += page->memory_footprint;
				++internal_pages;
			} else {
				leaf_bytes += page->memory_footprint;
				++leaf_pages;
			}
			WT_ERR(__wt_reconcile(session, walk, NULL, 0));
		}
		break;
	case WT_SYNC_CLOSE:
	case WT_SYNC_DISCARD:
		WT_ERR(__wt_illegal_value(session, NULL));
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
		    WT_TIMEDIFF_MS(end, start)));
	}

err:	/* On error, clear any left-over tree walk. */
	if (walk != NULL)
		WT_TRET(__wt_page_release(session, walk, flags));

	/*
	 * If we got a snapshot in order to write pages, and there was no
	 * snapshot active when we started, release it.
	 */
	if (txn->isolation == WT_ISO_READ_COMMITTED &&
	    saved_snap_min == WT_TXN_NONE)
		__wt_txn_release_snapshot(session);

	if (btree->checkpointing != WT_CKPT_OFF) {
		/*
		 * Update the checkpoint generation for this handle so visible
		 * updates newer than the checkpoint can be evicted.
		 *
		 * This has to be published before eviction is enabled again,
		 * so that eviction knows that the checkpoint has completed.
		 */
		WT_PUBLISH(btree->checkpoint_gen,
		    conn->txn_global.checkpoint_gen);
		WT_STAT_FAST_DATA_SET(session,
		    btree_checkpoint_generation, btree->checkpoint_gen);

		/*
		 * Clear the checkpoint flag and push the change; not required,
		 * but publishing the change means stalled eviction gets moving
		 * as soon as possible.
		 */
		btree->checkpointing = WT_CKPT_OFF;
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
	if (ret == 0 &&
	    syncop == WT_SYNC_WRITE_LEAVES && F_ISSET(conn, WT_CONN_CKPT_SYNC))
		WT_RET(btree->bm->sync(btree->bm, session, false));

	return (ret);
}

/*
 * __wt_cache_op --
 *	Cache operations.
 */
int
__wt_cache_op(WT_SESSION_IMPL *session, WT_CACHE_OP op)
{
	WT_DECL_RET;

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_CLOSE:
		/*
		 * Make sure the checkpoint reference is set for
		 * reconciliation; it's ugly, but drilling a function parameter
		 * path from our callers to the reconciliation of the tree's
		 * root page is going to be worse.
		 */
		WT_ASSERT(session, S2BT(session)->ckpt != NULL);
		break;
	case WT_SYNC_DISCARD:
	case WT_SYNC_WRITE_LEAVES:
		break;
	}

	switch (op) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_WRITE_LEAVES:
		ret = __sync_file(session, op);
		break;
	case WT_SYNC_CLOSE:
	case WT_SYNC_DISCARD:
		ret = __wt_evict_file(session, op);
		break;
	}
	return (ret);
}
