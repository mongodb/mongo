/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ref_index_slot --
 *      Return the page's index and slot for a reference.
 */
static inline void
__ref_index_slot(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_PAGE_INDEX **pindexp, uint32_t *slotp)
{
	WT_PAGE_INDEX *pindex;
	WT_REF **start, **stop, **p, **t;
	uint64_t sleep_count, yield_count;
	uint32_t entries, slot;

	/*
	 * If we don't find our reference, the page split and our home
	 * pointer references the wrong page. When internal pages
	 * split, their WT_REF structure home values are updated; yield
	 * and wait for that to happen.
	 */
	for (sleep_count = yield_count = 0;;) {
		/*
		 * Copy the parent page's index value: the page can split at
		 * any time, but the index's value is always valid, even if
		 * it's not up-to-date.
		 */
		WT_INTL_INDEX_GET(session, ref->home, pindex);
		entries = pindex->entries;

		/*
		 * Use the page's reference hint: it should be correct unless
		 * there was a split or delete in the parent before our slot.
		 * If the hint is wrong, it can be either too big or too small,
		 * but often only by a small amount.  Search up and down the
		 * index starting from the hint.
		 *
		 * It's not an error for the reference hint to be wrong, it
		 * just means the first retrieval (which sets the hint for
		 * subsequent retrievals), is slower.
		 */
		slot = ref->pindex_hint;
		if (slot >= entries)
			slot = entries - 1;
		if (pindex->index[slot] == ref)
			goto found;
		for (start = &pindex->index[0],
		    stop = &pindex->index[entries - 1],
		    p = t = &pindex->index[slot];
		    p > start || t < stop;) {
			if (p > start && *--p == ref) {
				slot = (uint32_t)(p - start);
				goto found;
			}
			if (t < stop && *++t == ref) {
				slot = (uint32_t)(t - start);
				goto found;
			}
		}
		/*
		 * We failed to get the page index and slot reference, yield
		 * before retrying, and if we've yielded enough times, start
		 * sleeping so we don't burn CPU to no purpose.
		 */
		__wt_ref_state_yield_sleep(&yield_count, &sleep_count);
		WT_STAT_CONN_INCRV(session, page_index_slot_ref_blocked,
		    sleep_count);
	}

found:	WT_ASSERT(session, pindex->index[slot] == ref);
	*pindexp = pindex;
	*slotp = slot;
}

/*
 * __ref_is_leaf --
 *	Check if a reference is for a leaf page.
 */
static inline bool
__ref_is_leaf(WT_REF *ref)
{
	size_t addr_size;
	const uint8_t *addr;
	u_int type;

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
 * __ref_ascend --
 *	Ascend the tree one level.
 */
static inline void
__ref_ascend(WT_SESSION_IMPL *session,
    WT_REF **refp, WT_PAGE_INDEX **pindexp, uint32_t *slotp)
{
	WT_REF *parent_ref, *ref;

	/*
	 * Ref points to the first/last slot on an internal page from which we
	 * are ascending the tree, moving to the parent page. This is tricky
	 * because the internal page we're on may be splitting into its parent.
	 * Find a stable configuration where the page we start from and the
	 * page we're moving to are connected. The tree eventually stabilizes
	 * into that configuration, keep trying until we succeed.
	 */
	for (ref = *refp;;) {
		/*
		 * Find our parent slot on the next higher internal page, the
		 * slot from which we move to a next/prev slot, checking that
		 * we haven't reached the root.
		 */
		parent_ref = ref->home->pg_intl_parent_ref;
		if (__wt_ref_is_root(parent_ref))
			break;
		__ref_index_slot(session, parent_ref, pindexp, slotp);

		/*
		 * There's a split race when a cursor moving forwards through
		 * the tree ascends the tree. If we're splitting an internal
		 * page into its parent, we move the WT_REF structures and
		 * then update the parent's page index before updating the split
		 * page's page index, and it's not an atomic update. A thread
		 * can read the split page's original page index and then read
		 * the parent page's replacement index.
		 *
		 * This can create a race for next-cursor movements.
		 *
		 * For example, imagine an internal page with 3 child pages,
		 * with the namespaces a-f, g-h and i-j; the first child page
		 * splits. The parent starts out with the following page-index:
		 *
		 *	| ... | a | g | i | ... |
		 *
		 * which changes to this:
		 *
		 *	| ... | a | c | e | g | i | ... |
		 *
		 * The split page starts out with the following page-index:
		 *
		 *	| a | b | c | d | e | f |
		 *
		 * Imagine a cursor finishing the 'f' part of the namespace that
		 * starts its ascent to the parent's 'a' slot. Then the page
		 * splits and the parent page's page index is replaced. If the
		 * cursor then searches the parent's replacement page index for
		 * the 'a' slot, it finds it and then increments to the slot
		 * after the 'a' slot, the 'c' slot, and then it incorrectly
		 * repeats its traversal of part of the namespace.
		 *
		 * This function takes a WT_REF argument which is the page from
		 * which we start our ascent. If the parent's slot we find in
		 * our search doesn't point to the same page as that initial
		 * WT_REF, there's a race and we start over again.
		 */
		if (ref->home == parent_ref->page)
			break;
	}

	*refp = parent_ref;
}

/*
 * __ref_descend_prev --
 *	Descend the tree one level, during a previous-cursor walk.
 */
static inline void
__ref_descend_prev(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX **pindexp)
{
	WT_PAGE_INDEX *pindex;
	uint64_t yield_count;

	/*
	 * We're passed a child page into which we're descending, and on which
	 * we have a hazard pointer.
	 */
	for (yield_count = 0;; yield_count++, __wt_yield()) {
		/*
		 * There's a split race when a cursor moving backwards through
		 * the tree descends the tree. If we're splitting an internal
		 * page into its parent, we move the WT_REF structures and
		 * update the parent's page index before updating the split
		 * page's page index, and it's not an atomic update. A thread
		 * can read the parent page's replacement page index and then
		 * read the split page's original index.
		 *
		 * This can create a race for previous-cursor movements.
		 *
		 * For example, imagine an internal page with 3 child pages,
		 * with the namespaces a-f, g-h and i-j; the first child page
		 * splits. The parent starts out with the following page-index:
		 *
		 *	| ... | a | g | i | ... |
		 *
		 * The split page starts out with the following page-index:
		 *
		 *	| a | b | c | d | e | f |
		 *
		 * The first step is to move the c-f ranges into a new subtree,
		 * so, for example we might have two new internal pages 'c' and
		 * 'e', where the new 'c' page references the c-d namespace and
		 * the new 'e' page references the e-f namespace. The top of the
		 * subtree references the parent page, but until the parent's
		 * page index is updated, any threads in the subtree won't be
		 * able to ascend out of the subtree. However, once the parent
		 * page's page index is updated to this:
		 *
		 *	| ... | a | c | e | g | i | ... |
		 *
		 * threads in the subtree can ascend into the parent. Imagine a
		 * cursor in the c-d part of the namespace that ascends to the
		 * parent's 'c' slot. It would then decrement to the slot before
		 * the 'c' slot, the 'a' slot.
		 *
		 * The previous-cursor movement selects the last slot in the 'a'
		 * page; if the split page's page-index hasn't been updated yet,
		 * it will select the 'f' slot, which is incorrect. Once the
		 * split page's page index is updated to this:
		 *
		 *	| a | b |
		 *
		 * the previous-cursor movement will select the 'b' slot, which
		 * is correct.
		 *
		 * This function takes an argument which is the internal page
		 * from which we're descending. If the last slot on the page no
		 * longer points to the current page as its "home", the page is
		 * being split and part of its namespace moved. We have the
		 * correct page and we don't have to move, all we have to do is
		 * wait until the split page's page index is updated.
		 */
		WT_INTL_INDEX_GET(session, ref->page, pindex);
		if (pindex->index[pindex->entries - 1]->home == ref->page)
			break;
	}
	*pindexp = pindex;
	WT_STAT_CONN_INCRV(session, tree_descend_blocked, yield_count);
}

/*
 * __ref_initial_descent_prev --
 *	Descend the tree one level, when setting up the initial cursor position
 * for a previous-cursor walk.
 */
static inline bool
__ref_initial_descent_prev(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX **pindexp)
{
	WT_PAGE_INDEX *pindex;

	/*
	 * We're passed a child page into which we're descending, and on which
	 * we have a hazard pointer.
	 *
	 * Acquire a page index for the child page and then confirm we haven't
	 * raced with a parent split.
	 */
	WT_INTL_INDEX_GET(session, ref->page, pindex);
	if (__wt_split_descent_race(session, ref, *pindexp))
		return (false);

	*pindexp = pindex;
	return (true);
}

/*
 * __tree_walk_internal --
 *	Move to the next/previous page in the tree.
 */
static inline int
__tree_walk_internal(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *walkcntp,
    int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool *),
    void *func_cookie, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE_INDEX *pindex;
	WT_REF *couple, *couple_orig, *ref;
	uint32_t current_state, slot;
	bool empty_internal, initial_descent, prev, skip;

	btree = S2BT(session);
	pindex = NULL;
	empty_internal = initial_descent = false;

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
	 * Clear the returned value, it makes future error handling easier.
	 */
	couple = couple_orig = ref = *refp;
	*refp = NULL;

	/* If no page is active, begin a walk from the start/end of the tree. */
	if (ref == NULL) {
restart:	/*
		 * We can be here with a NULL or root WT_REF; the page release
		 * function handles them internally, don't complicate this code
		 * by calling them out.
		 */
		WT_ERR(__wt_page_release(session, couple, flags));

		/*
		 * We're not supposed to walk trees without root pages. As this
		 * has not always been the case, assert to debug that change.
		 */
		WT_ASSERT(session, btree->root.page != NULL);

		couple = couple_orig = ref = &btree->root;
		initial_descent = true;
		goto descend;
	}

	/*
	 * If the active page was the root, we've reached the walk's end; we
	 * only get here if we've returned the root to our caller, so we're
	 * holding no hazard pointers.
	 */
	if (__wt_ref_is_root(ref))
		goto done;

	/* Figure out the current slot in the WT_REF array. */
	__ref_index_slot(session, ref, &pindex, &slot);

	for (;;) {
		/*
		 * If we're at the last/first slot on the internal page, return
		 * it in post-order traversal. Otherwise move to the next/prev
		 * slot and left/right-most element in that subtree.
		 */
		while ((prev && slot == 0) ||
		    (!prev && slot == pindex->entries - 1)) {
			/* Ascend to the parent. */
			__ref_ascend(session, &ref, &pindex, &slot);

			/*
			 * If at the root and returning internal pages, return
			 * the root page, otherwise we're done. Regardless, no
			 * hazard pointer is required, release the one we hold.
			 */
			if (__wt_ref_is_root(ref)) {
				WT_ERR(__wt_page_release(
				    session, couple, flags));
				if (!LF_ISSET(WT_READ_SKIP_INTL))
					*refp = ref;
				goto done;
			}

			/*
			 * If we got all the way through an internal page and
			 * all of the child pages were deleted, mark it for
			 * eviction.
			 */
			if (empty_internal && pindex->entries > 1) {
				__wt_page_evict_soon(session, ref);
				empty_internal = false;
			}

			/*
			 * Optionally return internal pages. Swap our previous
			 * hazard pointer for the page we'll return. We don't
			 * handle restart or not-found returns, it would require
			 * additional complexity and is not a possible return:
			 * we're moving to the parent of the current child page,
			 * the parent can't have been evicted.
			 */
			if (!LF_ISSET(WT_READ_SKIP_INTL)) {
				WT_ERR(__wt_page_swap(
				    session, couple, ref, flags));
				*refp = ref;
				goto done;
			}
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
			 * always update the hints when splitting, it's expected
			 * for them to be incorrect in some workloads.
			 */
			ref = pindex->index[slot];
			if (ref->pindex_hint != slot)
				ref->pindex_hint = slot;

			/*
			 * If we see any child states other than deleted, the
			 * page isn't empty.
			 */
			current_state = ref->state;
			if (current_state != WT_REF_DELETED &&
			    !LF_ISSET(WT_READ_TRUNCATE))
				empty_internal = false;

			if (LF_ISSET(WT_READ_CACHE)) {
				/*
				 * Only look at unlocked pages in memory:
				 * fast-path some common cases.
				 */
				if (LF_ISSET(WT_READ_NO_WAIT) &&
				    current_state != WT_REF_MEM &&
				    current_state != WT_REF_LIMBO)
					break;

				/* Skip lookaside pages if not requested. */
				if (current_state == WT_REF_LOOKASIDE &&
				    !LF_ISSET(WT_READ_LOOKASIDE))
					break;
			} else if (LF_ISSET(WT_READ_TRUNCATE)) {
				/*
				 * Avoid pulling a deleted page back in to try
				 * to delete it again.
				 */
				if (current_state == WT_REF_DELETED &&
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
			} else if (skip_func != NULL) {
				WT_ERR(skip_func(session,
				    ref, func_cookie, &skip));
				if (skip)
					break;
			} else {
				/*
				 * Try to skip deleted pages visible to us.
				 */
				if (current_state == WT_REF_DELETED &&
				    __wt_delete_page_skip(session, ref, false))
					break;
			}

			ret = __wt_page_swap(session, couple, ref,
			    WT_READ_NOTFOUND_OK | WT_READ_RESTART_OK | flags);

			/*
			 * Not-found is an expected return when only walking
			 * in-cache pages, or if we see a deleted page.
			 */
			if (ret == WT_NOTFOUND) {
				ret = 0;
				WT_NOT_READ(ret);
				break;
			}

			/*
			 * The page we're moving to might have split, in which
			 * case move to the last position we held.
			 */
			if (ret == WT_RESTART) {
				ret = 0;

				/*
				 * If a cursor is setting up at the end of the
				 * tree, we can't use our parent page's index,
				 * because it may have already split; restart
				 * the walk.
				 */
				if (prev && initial_descent)
					goto restart;

				/*
				 * If a new walk that never coupled from the
				 * root to a new saved position in the tree,
				 * restart the walk.
				 */
				if (couple == &btree->root)
					goto restart;

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
				__ref_index_slot(session, ref, &pindex, &slot);
				if (couple == couple_orig)
					break;
			}
			WT_ERR(ret);
			couple = ref;

			/*
			 * A new page: configure for traversal of any internal
			 * page's children, else return the leaf page.
			 */
			if (WT_PAGE_IS_INTERNAL(ref->page)) {
descend:			empty_internal = true;

				/*
				 * There's a split race when a cursor is setting
				 * up at the end of the tree or moving backwards
				 * through the tree and descending a level. When
				 * splitting an internal page into its parent,
				 * we move the WT_REF structures and update the
				 * parent's page index before updating the split
				 * page's page index, and it's not an atomic
				 * update. A thread can read the parent page's
				 * replacement page index, then read the split
				 * page's original index, or the parent page's
				 * original and the split page's replacement.
				 *
				 * This isn't a problem for a cursor setting up
				 * at the start of the tree or moving forwards
				 * through the tree because we do right-hand
				 * splits on internal pages and the initial part
				 * of the split page's namespace won't change as
				 * part of a split. A thread reading the parent
				 * page's and split page's indexes will move to
				 * the same slot no matter what order of indexes
				 * are read.
				 *
				 * Handle a cursor setting up at the end of the
				 * tree or moving backwards through the tree.
				 */
				if (!prev) {
					WT_INTL_INDEX_GET(
					    session, ref->page, pindex);
					slot = 0;
				} else if (initial_descent) {
					if (!__ref_initial_descent_prev(
					    session, ref, &pindex))
						goto restart;
					slot = pindex->entries - 1;
				} else {
					__ref_descend_prev(
					    session, ref, &pindex);
					slot = pindex->entries - 1;
				}
				continue;
			}

			/*
			 * The tree-walk restart code knows we return any leaf
			 * page we acquire (never hazard-pointer coupling on
			 * after acquiring a leaf page), and asserts no restart
			 * happens while holding a leaf page. This page must be
			 * returned to our caller.
			 */
			*refp = ref;
			goto done;
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
	return (__tree_walk_internal(session, refp, NULL, NULL, NULL, flags));
}

/*
 * __wt_tree_walk_count --
 *	Move to the next/previous page in the tree, tracking how many
 * references were visited to get there.
 */
int
__wt_tree_walk_count(WT_SESSION_IMPL *session,
    WT_REF **refp, uint64_t *walkcntp, uint32_t flags)
{
	return (__tree_walk_internal(
	    session, refp, walkcntp, NULL, NULL, flags));
}

/*
 * __wt_tree_walk_custom_skip --
 *	Walk the tree calling a custom function to decide whether to skip refs.
 */
int
__wt_tree_walk_custom_skip(
    WT_SESSION_IMPL *session, WT_REF **refp,
    int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool *),
    void *func_cookie, uint32_t flags)
{
	return (__tree_walk_internal(
	    session, refp, NULL, skip_func, func_cookie, flags));
}

/*
 * __tree_walk_skip_count_callback --
 *	Optionally skip leaf pages.
 * When the skip-leaf-count variable is non-zero, skip some count of leaf
 * pages, then take the next leaf page we can.
 *
 * The reason to do some of this work here, is because we can look at the cell
 * and know it's a leaf page without reading it into memory. If this page is
 * disk-based, crack the cell to figure out it's a leaf page without reading
 * it.
 */
static int
__tree_walk_skip_count_callback(
    WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool *skipp)
{
	uint64_t *skipleafcntp;

	skipleafcntp = (uint64_t *)context;
	WT_ASSERT(session, skipleafcntp != NULL);

	/*
	 * Skip deleted pages visible to us.
	 */
	if (ref->state == WT_REF_DELETED &&
	    __wt_delete_page_skip(session, ref, false))
		*skipp = true;
	else if (*skipleafcntp > 0 && __ref_is_leaf(ref)) {
		--*skipleafcntp;
		*skipp = true;
	} else
		*skipp = false;
	return (0);
}

/*
 * __wt_tree_walk_skip --
 *	Move to the next/previous page in the tree, skipping a certain number
 *	of leaf pages before returning.
 */
int
__wt_tree_walk_skip(
    WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *skipleafcntp)
{
	/*
	 * Optionally skip leaf pages, the second half. The tree-walk function
	 * didn't have an on-page cell it could use to figure out if the page
	 * was a leaf page or not, it had to acquire the hazard pointer and look
	 * at the page. The tree-walk code never acquires a hazard pointer on a
	 * leaf page without returning it, and it's not trivial to change that.
	 * So, the tree-walk code returns all leaf pages here and we deal with
	 * decrementing the count.
	 */
	do {
		WT_RET(__tree_walk_internal(session, refp, NULL,
		    __tree_walk_skip_count_callback, skipleafcntp,
		    WT_READ_NO_GEN | WT_READ_SKIP_INTL | WT_READ_WONT_NEED));

		/*
		 * The walk skipped internal pages, any page returned must be a
		 * leaf page.
		 */
		if (*skipleafcntp > 0)
			--*skipleafcntp;
	} while (*skipleafcntp > 0);

	return (0);
}
