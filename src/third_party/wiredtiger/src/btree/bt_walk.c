/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_tree_walk --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *walkcntp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *couple, *couple_orig, *ref;
	int prev, skip;
	uint32_t slot;

	btree = S2BT(session);

	/*
	 * Tree walks are special: they look inside page structures that splits
	 * may want to free.  Publish that the tree is active during this
	 * window.
	 */
	WT_ENTER_PAGE_INDEX(session);

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
	couple = couple_orig = ref = *refp;
	*refp = NULL;

	/* If no page is active, begin a walk from the start of the tree. */
	if (ref == NULL) {
		ref = &btree->root;
		if (ref->page == NULL)
			goto done;
		goto descend;
	}

ascend:	/*
	 * If the active page was the root, we've reached the walk's end.
	 * Release any hazard-pointer we're holding.
	 */
	if (__wt_ref_is_root(ref)) {
		WT_ERR(__wt_page_release(session, couple, flags));
		goto done;
	}

	/* Figure out the current slot in the WT_REF array. */
	__wt_page_refp(session, ref, &pindex, &slot);

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
				WT_ERR(__wt_page_release(
				    session, couple, flags));
			else {
				/*
				 * Locate the reference to our parent page then
				 * swap our child hazard pointer for the parent.
				 * We don't handle restart or not-found returns.
				 * It would require additional complexity and is
				 * not a possible return: we're moving to the
				 * parent of the current child page, our parent
				 * reference can't have split or been evicted.
				 */
				__wt_page_refp(session, ref, &pindex, &slot);
				if ((ret = __wt_page_swap(
				    session, couple, ref, flags)) != 0) {
					WT_TRET(__wt_page_release(
					    session, couple, flags));
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

		if (walkcntp != NULL)
			++*walkcntp;

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
				 * Avoid pulling a deleted page back in to try
				 * to delete it again.
				 */
				if (ref->state == WT_REF_DELETED &&
				    __wt_delete_page_skip(session, ref))
					break;
				/*
				 * If deleting a range, try to delete the page
				 * without instantiating it.
				 */
				WT_ERR(__wt_delete_page(session, ref, &skip));
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
				 * Try to skip deleted pages visible to us.
				 */
				if (ref->state == WT_REF_DELETED &&
				    __wt_delete_page_skip(session, ref))
					break;
			}

			ret = __wt_page_swap(session, couple, ref, flags);

			/*
			 * Not-found is an expected return when only walking
			 * in-cache pages.
			 */
			if (ret == WT_NOTFOUND) {
				ret = 0;
				break;
			}

			/*
			 * The page we're moving to might have split, in which
			 * case move to the last position we held.
			 */
			if (ret == WT_RESTART) {
				ret = 0;

				/*
				 * If a new walk that never coupled from the
				 * root to a new saved position in the tree,
				 * restart the walk.
				 */
				if (couple == &btree->root) {
					ref = &btree->root;
					if (ref->page == NULL)
						goto done;
					goto descend;
				}

				/*
				 * If restarting from some original position,
				 * repeat the increment or decrement we made at
				 * that time. Otherwise, couple is an internal
				 * page we've acquired after moving from that
				 * starting position and we can treat it as a
				 * new page. This works because we never acquire
				 * a hazard pointer on a leaf page we're not
				 * going to return to our caller, this will quit
				 * work if that ever changes.
				 */
				WT_ASSERT(session,
				    couple == couple_orig ||
				    WT_PAGE_IS_INTERNAL(couple->page));
				ref = couple;
				__wt_page_refp(session, ref, &pindex, &slot);
				if (couple == couple_orig)
					break;
			}
			WT_ERR(ret);

			/*
			 * A new page: configure for traversal of any internal
			 * page's children, else return the leaf page.
			 */
descend:		couple = ref;
			page = ref->page;
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_COL_INT) {
				pindex = WT_INTL_INDEX_COPY(page);
				slot = prev ? pindex->entries - 1 : 0;
			} else {
				*refp = ref;
				goto done;
			}
		}
	}

done:
err:	WT_LEAVE_PAGE_INDEX(session);
	return (ret);
}
