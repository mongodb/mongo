/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __tree_walk_fast --
 *	Fast delete for leaf pages that don't reference overflow items.
 */
static inline int
__tree_walk_fast(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, int *fast)
{
	WT_CELL_UNPACK unpack;

	*fast = 0;

	/*
	 * If we're discarding pages and the page is not in-memory (so no other
	 * thread is using it), and the page doesn't reference overflow items,
	 * (so there's no cleanup to do), try and simply delete the page.
	 */
	if (ref->state != WT_REF_DISK)
		return (0);

	__wt_cell_unpack(ref->addr, &unpack);
	if (unpack.raw != WT_CELL_ADDR_LNO)
		return (0);

	if (!WT_ATOMIC_CAS(ref->state, WT_REF_DISK, WT_REF_DELETED))
		return (0);

	/*
	 * This action dirtied the page: mark it dirty now, because there's no
	 * future reconciliation of a leaf page that will dirty it as we write
	 * the tree.
	 */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(page);

	*fast = 1;
	return (0);
}

/*
 * __wt_tree_walk --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session, WT_PAGE **pagep, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page, *t;
	WT_REF *ref;
	uint32_t slot;
	int discard, eviction, fast, prev;

	btree = session->btree;

	/* We can currently only do fast-discard on row-store trees. */
	discard = LF_ISSET(WT_TREE_DISCARD) && btree->type == BTREE_ROW ? 1 : 0;
	eviction = LF_ISSET(WT_TREE_EVICT) ? 1 : 0;
	prev = LF_ISSET(WT_TREE_PREV) ? 1 : 0;

	/*
	 * Take a copy of any returned page; we have a hazard reference on the
	 * page, by definition.
	 */
	page = *pagep;
	*pagep = NULL;

	/* If no page is active, begin a walk from the start of the tree. */
	if (page == NULL) {
		if ((page = btree->root_page) == NULL)
			return (0);
		slot = prev ? page->entries - 1 : 0;
		goto descend;
	}

	/* If the active page was the root, we've reached the walk's end. */
	if (WT_PAGE_IS_ROOT(page))
		return (0);

	/* Figure out the current slot in the parent page's WT_REF array. */
	t = page->parent;
	slot = (uint32_t)(page->ref - t->u.intl.t);

	/*
	 * Swap our hazard reference for the hazard reference of our parent,
	 * if it's not the root page (we could access it directly because we
	 * know it's in memory, but we need a hazard reference).  Don't leave
	 * a hazard reference dangling on error.
	 *
	 * We're hazard-reference coupling up the tree and that's OK: first,
	 * hazard references can't deadlock, so there's none of the usual
	 * problems found when logically locking up a Btree; second, we don't
	 * release our current hazard reference until we have our parent's
	 * hazard reference.  If the eviction thread tries to evict the active
	 * page, that fails because of our hazard reference.  If eviction tries
	 * to evict our parent, that fails because the parent has a child page
	 * that can't be discarded.
	 */
	if (eviction) {
		if (page->ref->state == WT_REF_EVICT_WALK)
			page->ref->state = WT_REF_MEM;
	} else {
		if (!WT_PAGE_IS_ROOT(t))
			ret = __wt_page_in(session, t, t->ref);
		__wt_page_release(session, page);
		WT_RET(ret);
	}
	page = t;

	/*
	 * If we're at the last/first slot on the page, return this page in
	 * post-order traversal.  Otherwise we move to the next/prev slot
	 * and left/right-most element in its subtree.
	 */
	for (;;) {
		if ((prev && slot == 0) ||
		    (!prev && slot == page->entries - 1)) {
			*pagep = page;
			return (0);
		}
		if (prev)
			--slot;
		else
			++slot;

descend:	for (;;) {
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_COL_INT)
				ref = &page->u.intl.t[slot];
			else {
				*pagep = page;
				return (0);
			}

			/*
			 * The eviction server walks an in-memory tree for two
			 * reasons:
			 *
			 * (1) to sync a file (write all dirty pages); and
			 * (2) to find pages to evict.
			 *
			 * We want all ordinary in-memory pages, and we swap
			 * the state to WT_REF_EVICT_WALK temporarily to avoid
			 * the page being evicted by another thread while it is
			 * being evaluated.
			 *
			 * We also return pages in the "evict-force" state,
			 * which indicates they are waiting on the eviction
			 * server getting to a request.  A sync call in the
			 * meantime must write such a page to ensure all
			 * modifications are written.  Since this is happening
			 * inside the eviction server, and an LRU walk will
			 * check the state before adding the page to the LRU
			 * queue, there is no way for an evict-force page to
			 * disappear from under us.
			 */
			if (eviction) {
				if (!WT_ATOMIC_CAS(ref->state,
				    WT_REF_MEM, WT_REF_EVICT_WALK) &&
				    ref->state != WT_REF_EVICT_FORCE)
					break;
			} else {
				/* Skip already deleted pages. */
				if (ref->state == WT_REF_DELETED)
					break;

				if (discard) {
					WT_RET(__tree_walk_fast(
					    session, page, ref, &fast));
					if (fast)
						break;
				}

				/*
				 * Swap hazard references at each level (but
				 * don't leave a hazard reference dangling on
				 * error).
				 */
				ret = __wt_page_in(session, page, ref);
				__wt_page_release(session, page);
				WT_RET(ret);
			}

			page = ref->page;
			WT_ASSERT(session, page != NULL);
			slot = prev ? page->entries - 1 : 0;
		}
	}
	/* NOTREACHED */
}
