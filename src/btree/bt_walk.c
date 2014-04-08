/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_tree_walk_delete_rollback --
 *	Abort pages that were deleted without being instantiated.
 */
void
__wt_tree_walk_delete_rollback(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_PAGE *page;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/*
	 * If the page is still "deleted", it's as we left it, reset the state
	 * to on-disk and we're done.  Otherwise, we expect the page is either
	 * instantiated or being instantiated.  Loop because it's possible for
	 * the page to return to the deleted state if instantiation fails.
	 */
	for (;; __wt_yield())
		switch (ref->state) {
		case WT_REF_DISK:
		case WT_REF_LOCKED:
		case WT_REF_READING:
			break;
		case WT_REF_DELETED:
			if (WT_ATOMIC_CAS(
			    ref->state, WT_REF_DELETED, WT_REF_DISK))
				return;
			break;
		case WT_REF_MEM:
			/*
			 * We can't use the normal read path to get a copy of
			 * the page because the session may have closed the
			 * cursor, we no longer have the reference to the tree
			 * required for a hazard pointer.  We're safe because
			 * with unresolved transactions, the page isn't going
			 * anywhere.
			 *
			 * We don't care about any key/value items other than
			 * the ones read from disk, any other items modified
			 * after the page was read will have been separately
			 * entered into the transaction list.
			 */
			page = ref->page;
			WT_ROW_FOREACH(page, rip, i)
			for (upd = WT_ROW_UPDATE(page, rip);
			    upd != NULL; upd = upd->next)
				if (upd->txnid == ref->txnid)
					upd->txnid = WT_TXN_ABORTED;
			break;
		case WT_REF_SPLIT:
			/*
			 * This is possible and it's a really bad thing.
			 */
			WT_ASSERT(session, ref->state != WT_REF_SPLIT);
		}
}

/*
 * __tree_walk_delete --
 *	If deleting a range, try to delete the page without instantiating it.
 */
static inline int
__tree_walk_delete(WT_SESSION_IMPL *session, WT_REF *ref, int *skipp)
{
	WT_DECL_RET;
	WT_PAGE *parent;

	*skipp = 0;

	/*
	 * Atomically switch the page's state to lock it.  If the page is not
	 * on-disk, other threads may be using it, no fast delete.
	 *
	 * Possible optimization: if the page is already deleted and the delete
	 * is visible to us (the delete has been committed), we could skip the
	 * page instead of instantiating it and figuring out there are no rows
	 * in the page.  While that's a huge amount of work to no purpose, it's
	 * unclear optimizing for overlapping range deletes is worth the effort.
	 */
	if (ref->state != WT_REF_DISK ||
	    !WT_ATOMIC_CAS(ref->state, WT_REF_DISK, WT_REF_LOCKED))
		return (0);

	/*
	 * We cannot fast-delete pages that have overflow key/value items as
	 * the overflow blocks have to be discarded.  The way we figure that
	 * out is to check the on-page cell type for the page, cells for leaf
	 * pages that have no overflow items are special.
	 *
	 * In some cases, the reference address may not reference an on-page
	 * cell (for example, some combination of page splits), in which case
	 * we can't check the original cell value and we fail.
	 *
	 * To look at an on-page cell, we need to look at the parent page, and
	 * that's dangerous, our parent page could change without warning if
	 * the parent page were to split, deepening the tree.  It's safe: the
	 * page's reference will always point to some valid page, and if we find
	 * any problems we simply fail the fast-delete optimization.
	 *
	 * !!!
	 * I doubt it's worth the effort, but we could copy the cell's type into
	 * the reference structure, and then we wouldn't need an on-page cell.
	 */
	parent = ref->home;
	if (__wt_off_page(parent, ref->addr) ||
	    __wt_cell_type_raw(ref->addr) != WT_CELL_ADDR_LEAF_NO)
		goto err;

	/*
	 * Record the change in the transaction structure and set the change's
	 * transaction ID.
	 */
	WT_ERR(__wt_txn_modify_ref(session, ref));

	/*
	 * This action dirties the parent page: mark it dirty now, there's no
	 * future reconciliation of the child leaf page that will dirty it as
	 * we write the tree.
	 */
	WT_ERR(__wt_page_parent_modify_set(session, ref, 0));

	*skipp = 1;

	/* Delete the page. */
	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (0);

err:	/*
	 * Restore the page to on-disk status, we'll have to instantiate it.  We
	 * don't bother to back out adding this node to the transaction modify
	 * list: on rollback, the rollback function will walk the page looking
	 * for transactions to abort, which is potentially wasted work but won't
	 * hurt anything.
	 */
	WT_PUBLISH(ref->state, WT_REF_DISK);
	return (ret);
}

/*
 * __tree_walk_read --
 *	If iterating a cursor, skip deleted pages that are visible to us.
 */
static inline int
__tree_walk_read(WT_SESSION_IMPL *session, WT_REF *ref, int *skipp)
{
	*skipp = 0;

	/*
	 * Do a simple test first, avoid the atomic operation unless it's
	 * demonstrably necessary.
	 */
	if (ref->state != WT_REF_DELETED)
		return (0);

	/*
	 * It's possible the state is changing underneath us, we could race
	 * between checking for a deleted state and looking at the stored
	 * transaction ID to see if the delete is visible to us.  Lock down
	 * the structure.
	 */
	if (!WT_ATOMIC_CAS(ref->state, WT_REF_DELETED, WT_REF_LOCKED))
		return (0);

	*skipp = __wt_txn_visible(session, ref->txnid) ? 1 : 0;

	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (0);
}

/*
 * __wt_tree_walk --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *couple, *ref;
	WT_TXN_STATE *txn_state;
	int descending, prev, skip;
	uint32_t slot;

	btree = S2BT(session);
	descending = 0;

	/*
	 * !!!
	 * Fast-truncate currently only works on row-store trees.
	 */
	if (btree->type != BTREE_ROW)
		LF_CLR(WT_READ_TRUNCATE);

	prev = LF_ISSET(WT_READ_PREV) ? 1 : 0;

	/*
	 * Pin a transaction ID, required to safely look at page index
	 * structures, if our caller has not already done so.
	 */
	txn_state = WT_SESSION_TXN_STATE(session);
	if (txn_state->snap_min == WT_TXN_NONE)
		txn_state->snap_min = S2C(session)->txn_global.last_running;
	else
		txn_state = NULL;

	/*
	 * There are multiple reasons and approaches to walking the in-memory
	 * tree:
	 *
	 * (1) finding pages to evict (the eviction server);
	 * (2) writing just dirty leaves or internal nodes (checkpoint);
	 * (3) discarding pages (close);
	 * (4) truncating pages in a range (fast truncate);
	 * (5) skipping pages based on outside information (compaction);
	 * (6) cursor scans (applications).
	 *
	 * Except for cursor scans and compaction, the walk is limited to the
	 * cache, no pages are read.  In all cases, hazard pointers protect the
	 * walked pages from eviction.
	 *
	 * Walks use hazard-pointer coupling through the tree and that's OK
	 * (hazard pointers can't deadlock, so there's none of the usual
	 * problems found when logically locking up a btree).  If the eviction
	 * thread tries to evict the active page, it fails because of our
	 * hazard pointer.  If eviction tries to evict our parent, that fails
	 * because the parent has a child page that can't be discarded.  We do
	 * play one game: don't couple up to our parent and then back down to a
	 * new leaf, couple to the next page to which we're descending, it
	 * saves a hazard-pointer swap for each cursor page movement.
	 *
	 * !!!
	 * NOTE: we depend on the fact it's OK to release a page we don't hold,
	 * that is, it's OK to release couple when couple is set to NULL.
	 *
	 * Take a copy of any held page and clear the return value.  Remember
	 * the hazard pointer we're currently holding.
	 *
	 * We may be passed a pointer to btree->evict_page that we are clearing
	 * here.  We check when discarding pages that we're not discarding that
	 * page, so this clear must be done before the page is released.
	 */
	couple = ref = *refp;
	*refp = NULL;

	/* If no page is active, begin a walk from the start of the tree. */
	if (ref == NULL) {
		ref = &btree->root;
		if (ref->page == NULL) {
			if (txn_state != NULL)
				txn_state->snap_min = WT_TXN_NONE;
			goto done;
		}
		goto descend;
	}

ascend:	/*
	 * If the active page was the root, we've reached the walk's end.
	 * Release any hazard-pointer we're holding.
	 */
	if (__wt_ref_is_root(ref)) {
		WT_ERR(__wt_page_release(session, couple));
		goto done;
	}

	/* Figure out the current slot in the WT_REF array. */
	__wt_page_refp(session, ref, &pindex, &slot);

	if (0) {
restart:	/*
		 * The page we're moving to might have split, in which case find
		 * the last position we held.
		 *
		 * If we were starting a tree walk, begin again.
		 *
		 * If we were in the process of descending, repeat the descent.
		 * If we were moving within a single level of the tree, repeat
		 * the last move.
		 */
		ref = couple;
		if (ref == &btree->root) {
			ref = &btree->root;
			if (ref->page == NULL) {
				if (txn_state != NULL)
					txn_state->snap_min = WT_TXN_NONE;
				goto done;
			}
			goto descend;
		}
		__wt_page_refp(session, ref, &pindex, &slot);
		if (descending)
			goto descend;
	}

	for (;;) {
		/*
		 * If we're at the last/first slot on the page, return this page
		 * in post-order traversal.  Otherwise we move to the next/prev
		 * slot and left/right-most element in its subtree.
		 */
		if ((prev && slot == 0) ||
		    (!prev && slot == pindex->entries - 1)) {
			ref = ref->home->pg_intl_parent_ref;

			/* Optionally skip internal pages. */
			if (LF_ISSET(WT_READ_SKIP_INTL))
				goto ascend;

			/*
			 * We've ascended the tree and are returning an internal
			 * page.  If it's the root, discard our hazard pointer,
			 * otherwise, swap our hazard pointer for the page we'll
			 * return.
			 */
			if (__wt_ref_is_root(ref))
				WT_ERR(__wt_page_release(session, couple));
			else {
				/*
				 * Locate the reference to our parent page then
				 * swap our child hazard pointer for the parent.
				 * We don't handle a restart return because it
				 * would require additional complexity in the
				 * restart code (ascent code somewhat like the
				 * descent code already there), and it's not a
				 * possible return: we're moving to the parent
				 * of the current child, not another child of
				 * the same parent, there's no way our parent
				 * split.
				 */
				__wt_page_refp(session, ref, &pindex, &slot);
				if ((ret = __wt_page_swap(
				    session, couple, ref, flags)) != 0) {
					WT_TRET(
					    __wt_page_release(session, couple));
					WT_ERR(ret);
				}
			}

			*refp = ref;
			goto done;
		}

		if (prev)
			--slot;
		else
			++slot;

		for (descending = 0;;) {
			ref = pindex->index[slot];

			if (LF_ISSET(WT_READ_CACHE)) {
				/*
				 * Only look at unlocked pages in memory:
				 * fast-path some common cases.
				 */
				if (LF_ISSET(WT_READ_NO_WAIT) &&
				    ref->state != WT_REF_MEM)
					break;
			} else if (LF_ISSET(WT_READ_TRUNCATE)) {
				/*
				 * If deleting a range, try to delete the page
				 * without instantiating it.
				 */
				WT_ERR(__tree_walk_delete(
				    session, ref, &skip));
				if (skip)
					break;
			} else if (LF_ISSET(WT_READ_COMPACT)) {
				/*
				 * Skip deleted pages, rewriting them doesn't
				 * seem useful.
				 */
				if (ref->state == WT_REF_DELETED)
					break;

				/*
				 * If the page is in-memory, we want to look at
				 * it (it may have been modified and written,
				 * and the current location is the interesting
				 * one in terms of compaction, not the original
				 * location).  If the page isn't in-memory, test
				 * if the page will help with compaction, don't
				 * read it if we don't have to.
				 */
				if (ref->state == WT_REF_DISK) {
					WT_ERR(__wt_compact_page_skip(
					    session, ref, &skip));
					if (skip)
						break;
				}
			} else {
				/*
				 * If iterating a cursor, skip deleted pages
				 * that are visible to us.
				 */
				WT_ERR(__tree_walk_read(session, ref, &skip));
				if (skip)
					break;
			}

			ret = __wt_page_swap(session, couple, ref, flags);
			if (ret == WT_NOTFOUND) {
				ret = 0;
				break;
			}
			if (ret == WT_RESTART)
				goto restart;
			WT_ERR(ret);

			/*
			 * Entering a new page: configure for traversal of any
			 * internal page's children, else return (or optionally
			 * skip), the leaf page.
			 */
descend:		couple = ref;
			page = ref->page;
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_COL_INT) {
				pindex = WT_INTL_INDEX_COPY(page);
				slot = prev ? pindex->entries - 1 : 0;
				descending = 1;
			} else if (LF_ISSET(WT_READ_SKIP_LEAF))
				goto ascend;
			else {
				*refp = ref;
				goto done;
			}
		}
	}

done:
err:	if (txn_state != NULL)
		txn_state->snap_min = WT_TXN_NONE;
	return (ret);
}
