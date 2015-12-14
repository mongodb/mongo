/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __page_refp --
 *      Return the page's index and slot for a reference.
 */
static inline void
__page_refp(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_PAGE_INDEX **pindexp, uint32_t *slotp)
{
	WT_PAGE_INDEX *pindex;
	uint32_t i;

	/*
	 * Copy the parent page's index value: the page can split at any time,
	 * but the index's value is always valid, even if it's not up-to-date.
	 */
retry:	WT_INTL_INDEX_GET(session, ref->home, pindex);

	/*
	 * Use the page's reference hint: it should be correct unless the page
	 * split before our slot.  If the page splits after our slot, the hint
	 * will point earlier in the array than our actual slot, so the first
	 * loop is from the hint to the end of the list, and the second loop
	 * is from the start of the list to the end of the list.  (The second
	 * loop overlaps the first, but that only happen in cases where we've
	 * deepened the tree and aren't going to find our slot at all, that's
	 * not worth optimizing.)
	 *
	 * It's not an error for the reference hint to be wrong, it just means
	 * the first retrieval (which sets the hint for subsequent retrievals),
	 * is slower.
	 */
	i = ref->pindex_hint;
	if (i < pindex->entries && pindex->index[i]->page == ref->page) {
		*pindexp = pindex;
		*slotp = i;
		return;
	}
	while (++i < pindex->entries)
		if (pindex->index[i]->page == ref->page) {
			*pindexp = pindex;
			*slotp = ref->pindex_hint = i;
			return;
		}
	for (i = 0; i < pindex->entries; ++i)
		if (pindex->index[i]->page == ref->page) {
			*pindexp = pindex;
			*slotp = ref->pindex_hint = i;
			return;
		}

	/*
	 * If we don't find our reference, the page split into a new level and
	 * our home pointer references the wrong page.  After internal pages
	 * deepen, their reference structure home value are updated; yield and
	 * wait for that to happen.
	 */
	__wt_yield();
	goto retry;
}

/*
 * __ref_is_leaf --
 *	Check if a reference is for a leaf page.
 */
static inline bool
__ref_is_leaf(WT_REF *ref)
{
	size_t addr_size;
	u_int type;
	const uint8_t *addr;

	/*
	 * If the page has a disk address, we can crack it to figure out if
	 * this page is a leaf page or not. If there's no address, the page
	 * isn't on disk and we don't know the page type.
	 */
	__wt_ref_info(ref, &addr, &addr_size, &type);
	return (addr == NULL ?
	    false : type == WT_CELL_ADDR_LEAF || type == WT_CELL_ADDR_LEAF_NO);
}

/*
 * __tree_walk_internal --
 *	Move to the next/previous page in the tree.
 */
static inline int
__tree_walk_internal(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *walkcntp, uint64_t *skipleafcntp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE_INDEX *pindex;
	WT_REF *couple, *couple_orig, *ref;
	bool empty_internal, prev, skip;
	uint32_t slot;

	btree = S2BT(session);
	empty_internal = false;

	/*
	 * Tree walks are special: they look inside page structures that splits
	 * may want to free.  Publish that the tree is active during this
	 * window.
	 */
	WT_ENTER_PAGE_INDEX(session);

	/* Walk should never instantiate deleted pages. */
	LF_SET(WT_READ_NO_EMPTY);

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
	__page_refp(session, ref, &pindex, &slot);

	for (;;) {
		/*
		 * If we're at the last/first slot on the page, return this page
		 * in post-order traversal.  Otherwise we move to the next/prev
		 * slot and left/right-most element in its subtree.
		 */
		if ((prev && slot == 0) ||
		    (!prev && slot == pindex->entries - 1)) {
			ref = ref->home->pg_intl_parent_ref;

			/*
			 * If we got all the way through an internal page and
			 * all of the child pages were deleted, mark it for
			 * eviction.
			 */
			if (empty_internal && pindex->entries > 1) {
				__wt_page_evict_soon(ref->page);
				empty_internal = false;
			}

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
				__page_refp(session, ref, &pindex, &slot);
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
			/*
			 * Move to the next slot, and set the reference hint if
			 * it's wrong (used when we continue the walk). We don't
			 * update those hints when splitting, so it's common for
			 * them to be incorrect in some workloads.
			 */
			ref = pindex->index[slot];
			if (ref->pindex_hint != slot)
				ref->pindex_hint = slot;

			/*
			 * If we see any child states other than deleted, the
			 * page isn't empty.
			 */
			if (ref->state != WT_REF_DELETED &&
			    !LF_ISSET(WT_READ_TRUNCATE))
				empty_internal = false;

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
				    __wt_delete_page_skip(session, ref, false))
					break;
				/*
				 * If deleting a range, try to delete the page
				 * without instantiating it.
				 */
				WT_ERR(__wt_delete_page(session, ref, &skip));
				if (skip)
					break;
				empty_internal = false;
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
				    __wt_delete_page_skip(session, ref, false))
					break;
			}

			/*
			 * Optionally skip leaf pages: skip all leaf pages if
			 * WT_READ_SKIP_LEAF is set, when the skip-leaf-count
			 * variable is non-zero, skip some count of leaf pages.
			 * If this page is disk-based, crack the cell to figure
			 * out it's a leaf page without reading it.
			 *
			 * If skipping some number of leaf pages, decrement the
			 * count of pages to zero, and then take the next leaf
			 * page we can. Be cautious around the page decrement,
			 * if for some reason don't take this particular page,
			 * we can take the next one, and, there are additional
			 * tests/decrements when we're about to return a leaf
			 * page.
			 */
			if (skipleafcntp != NULL || LF_ISSET(WT_READ_SKIP_LEAF))
				if (__ref_is_leaf(ref)) {
					if (LF_ISSET(WT_READ_SKIP_LEAF))
						break;
					if (*skipleafcntp > 0) {
						--*skipleafcntp;
						break;
					}
				}

			ret = __wt_page_swap(session, couple, ref, flags);

			/*
			 * Not-found is an expected return when only walking
			 * in-cache pages, or if we see a deleted page.
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
				 * working if that ever changes.
				 */
				WT_ASSERT(session,
				    couple == couple_orig ||
				    WT_PAGE_IS_INTERNAL(couple->page));
				ref = couple;
				__page_refp(session, ref, &pindex, &slot);
				if (couple == couple_orig)
					break;
			}
			WT_ERR(ret);

			/*
			 * A new page: configure for traversal of any internal
			 * page's children, else return the leaf page.
			 */
			if (WT_PAGE_IS_INTERNAL(ref->page)) {
descend:			couple = ref;
				WT_INTL_INDEX_GET(session, ref->page, pindex);
				slot = prev ? pindex->entries - 1 : 0;
				empty_internal = true;
			} else {
				/*
				 * Optionally skip leaf pages, the second half.
				 * We didn't have an on-page cell to figure out
				 * if it was a leaf page, we had to acquire the
				 * hazard pointer and look at the page.
				 */
				if (skipleafcntp != NULL ||
				    LF_ISSET(WT_READ_SKIP_LEAF)) {
					couple = ref;
					if (LF_ISSET(WT_READ_SKIP_LEAF))
						break;
					if (*skipleafcntp > 0) {
						--*skipleafcntp;
						break;
					}
				}

				*refp = ref;
				goto done;
			}
		}
	}

done:
err:	WT_LEAVE_PAGE_INDEX(session);
	return (ret);
}

/*
 * __wt_tree_walk --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
	return (__tree_walk_internal(session, refp, NULL, NULL, flags));
}

/*
 * __wt_tree_walk_count --
 *	Move to the next/previous page in the tree, tracking how many
 *	references were visited to get there.
 */
int
__wt_tree_walk_count(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *walkcntp, uint32_t flags)
{
	return (__tree_walk_internal(session, refp, walkcntp, NULL, flags));
}

/*
 * __wt_tree_walk_skip --
 *	Move to the next/previous page in the tree, skipping a certain number
 *	of leaf pages before returning.
 */
int
__wt_tree_walk_skip(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *skipleafcntp, uint32_t flags)
{
	return (__tree_walk_internal(session, refp, NULL, skipleafcntp, flags));
}
