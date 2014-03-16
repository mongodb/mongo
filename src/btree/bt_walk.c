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
__wt_tree_walk_delete_rollback(WT_REF *ref)
{
	WT_PAGE *page;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/*
	 * If the page is still marked deleted, it's as we left it, reset the
	 * state to on-disk and we're done.
	 */
	if (WT_ATOMIC_CAS(ref->state, WT_REF_DELETED, WT_REF_DISK))
		return;

	/*
	 * The page is either instantiated or being instantiated -- wait for
	 * the page to settle down, as needed, and then clean up the update
	 * structures.  We don't need a hazard pointer or anything on the
	 * page because there are unresolved transactions, the page can't go
	 * anywhere.
	 */
	while (ref->state != WT_REF_MEM)
		__wt_yield();
	page = ref->page;
	WT_ROW_FOREACH(page, rip, i)
		for (upd =
		    WT_ROW_UPDATE(page, rip); upd != NULL; upd = upd->next)
			if (upd->txnid == ref->txnid)
				upd->txnid = WT_TXN_ABORTED;
}

/*
 * __tree_walk_delete --
 *	If deleting a range, try to delete the page without instantiating it.
 */
static inline int
__tree_walk_delete(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, int *skipp)
{
	WT_DECL_RET;

	*skipp = 0;

	/*
	 * If the page is already instantiated in-memory, other threads may be
	 * using it, no fast delete.
	 *
	 * Atomically switch the page's state to lock it.  If the page state
	 * changes underneath us, no fast delete.
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
	 * XXXKEITH: this may no longer be possible, review.
	 *
	 * We may be in a reconciliation-built internal page if the page split.
	 * In that case, the reference address doesn't point to a cell.  While
	 * we could probably still fast-delete the page, I doubt it's a common
	 * enough case to make it worth the effort.
	 */
	if (__wt_off_page(page, ref->addr))
		goto err;

	/*
	 * If the page references overflow items, we have to clean it up during
	 * reconciliation, no fast delete.   Check this after we have the page
	 * locked down, instantiating the page in memory and modifying it could
	 * theoretically point the address somewhere away from the on-page cell.
	 */
	if (__wt_cell_type_raw(ref->addr) != WT_CELL_ADDR_LEAF_NO)
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
	WT_ERR(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	*skipp = 1;

	/* Delete the page. */
	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (0);

err:	/*
	 * Restore the page to on-disk status, we'll have to instantiate it.
	 * We're don't have to back out adding this node to the transaction
	 * modify list, that's OK because the rollback function ignores nodes
	 * that aren't set to WT_REF_DELETED.
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
 * PAGE_SWAP --
 *	Macro to swap pages, handling the split restart.
 */
#undef	PAGE_SWAP
#define	PAGE_SWAP(session, couple, page, ref, flags, ret) do {		\
	if (((ret) = __wt_page_swap(					\
	    session, couple, page, ref, flags)) == WT_RESTART)		\
		goto restart;						\
} while (0)

/*
 * __wt_tree_walk --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *couple, *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref;
	uint32_t page_entries, slot;
	int prev, skip;

	btree = S2BT(session);

	/*
	 * !!!
	 * Fast-truncate currently only works on row-store trees.
	 */
	if (btree->type != BTREE_ROW)
		LF_CLR(WT_READ_TRUNCATE);

	prev = LF_ISSET(WT_READ_PREV) ? 1 : 0;

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
	couple = page = *pagep;
	*pagep = NULL;

	/* If no page is active, begin a walk from the start of the tree. */
	if (page == NULL) {
		if ((page = btree->root_page) == NULL)
			return (0);
		goto descend;
	}

ascend:	/*
	 * If the active page was the root, we've reached the walk's end.
	 * Release any hazard-pointer we're holding.
	 */
	if (WT_PAGE_IS_ROOT(page))
		return (__wt_page_release(session, couple));

	/*
	 * Figure out the current slot in the parent page's WT_REF array and
	 * switch to the parent.
	 */
	WT_RET(__wt_page_refp(session, page, &pindex, &slot));
	page_entries = pindex->entries;
	ref = pindex->index[slot];
	page = page->parent;

	for (;;) {
		/*
		 * If we're at the last/first slot on the page, return this
		 * page in post-order traversal.  Otherwise we move to the
		 * next/prev slot and left/right-most element in its subtree.
		 */
		if ((prev && slot == 0) ||
		    (!prev && slot == page_entries - 1)) {
			/* Optionally skip internal pages. */
			if (LF_ISSET(WT_READ_SKIP_INTL))
				goto ascend;

			/*
			 * We've ascended the tree and are returning an internal
			 * page.  If it's the root, discard any hazard pointer
			 * we have, otherwise, swap any hazard pointer we have
			 * for the page we'll return.  We could keep the hazard
			 * pointer we have as it's sufficient to pin any page in
			 * our page stack, but we have no place to store it and
			 * it's simpler if callers just know they hold a hazard
			 * pointer on any page they're using.
			 *
			 * XXXKEITH:
			 * Can this page-swap function return restart because of
			 * a page split?  I don't think so (this is an internal
			 * page that's pinned in memory), but I'm not 100% sure.
			 * Note we're not handling a return of WT_RESTART.
			 */
			if (WT_PAGE_IS_ROOT(page))
				WT_RET(__wt_page_release(session, couple));
			else {
				ref = __wt_page_ref(session, page);
				ret = __wt_page_swap(
				    session, couple, page, ref, flags);
				if (ret != 0)
					page = NULL;
				if (ret == WT_NOTFOUND)
					ret =
					    __wt_page_release(session, couple);
			}

			*pagep = page;
			return (ret);
		}

		if (0) {
restart:		/*
			 * The page we're moving to might have split, in which
			 * case use the last page we had to locate the cursor
			 * in the newly split tree and repeat the last move.
			 * If we don't have a place to stand, we must have been
			 * starting a tree walk, begin again.
			 */
			if (couple == NULL) {
				if ((page = btree->root_page) == NULL)
					return (0);
				goto descend;
			}
			WT_RET(__wt_page_refp(session, couple, &pindex, &slot));
			page_entries = pindex->entries;
		}

		if (prev)
			--slot;
		else
			++slot;

		for (;;) {
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
				WT_RET(__tree_walk_delete(
				    session, page, ref, &skip));
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
					WT_RET(__wt_compact_page_skip(
					    session, page, ref, &skip));
					if (skip)
						break;
				}
			} else {
				/*
				 * If iterating a cursor, skip deleted pages
				 * that are visible to us.
				 */
				WT_RET(__tree_walk_read(session, ref, &skip));
				if (skip)
					break;
			}

			PAGE_SWAP(session, couple, page, ref, flags, ret);
			if (ret == WT_NOTFOUND) {
				ret = 0;
				break;
			}
			WT_RET(ret);

			/*
			 * Entering a new page: configure for traversal of any
			 * internal page's children, else return (or optionally
			 * skip), the leaf page.
			 */
			couple = page = ref->page;
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_COL_INT) {
descend:			pindex = page->pg_intl_index;
				page_entries = pindex->entries;
				slot = prev ? page_entries - 1 : 0;
			} else if (LF_ISSET(WT_READ_SKIP_LEAF))
				goto ascend;
			else {
				*pagep = page;
				return (0);
			}
		}
	}
	/* NOTREACHED */
}
