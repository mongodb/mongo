/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_tree_np --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_np(WT_SESSION_IMPL *session, WT_PAGE **pagep, int cacheonly, int next)
{
	WT_BTREE *btree;
	WT_PAGE *page, *t;
	WT_REF *ref;
	uint32_t slot;
	int ret;

	btree = session->btree;

	/*
	 * Take a copy of any returned page; we have a hazard reference on the
	 * page, by definition.
	 */
	page = *pagep;
	*pagep = NULL;

	/* If no page is active, begin a walk from the start of the tree. */
	if (page == NULL) {
		if ((page = btree->root_page.page) == NULL)
			return (0);
		slot = next ? 0 : page->entries - 1;
		goto descend;
	}

	/* If the active page was the root, we've reached the walk's end. */
	if (WT_PAGE_IS_ROOT(page))
		return (0);

	/* Figure out the current slot in the parent page. */
	t = page->parent;
	slot =
	    page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_ROW_LEAF ?
	    WT_ROW_REF_SLOT(t, page->parent_ref) :
	    WT_COL_REF_SLOT(t, page->parent_ref);

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
	ret = WT_PAGE_IS_ROOT(t) ?
	    0 : __wt_page_in(session, t, t->parent_ref, 0);
	__wt_page_release(session, page);
	WT_RET(ret);
	page = t;

	/*
	 * If we're at the last/first slot on the page, return this page in
	 * post-order traversal.  Otherwise we move to the next/prev slot
	 * and left/right-most element in its subtree.
	 */
	for (;;) {
		if ((!next && slot == 0) ||
		    (next && slot == page->entries - 1)) {
			*pagep = page;
			return (0);
		}
		if (next)
			++slot;
		else
			--slot;

descend:	for (;;) {
			if (page->type == WT_PAGE_ROW_INT)
				ref = &page->u.row_int.t[slot].ref;
			else if (page->type == WT_PAGE_COL_INT)
				ref = &page->u.col_int.t[slot].ref;
			else {
				*pagep = page;
				return (0);
			}

			/* We may only care about in-memory pages. */
			if (cacheonly && ref->state != WT_REF_MEM)
				break;

			/*
			 * Swap hazard references at each level (but don't
			 * leave a hazard reference dangling on error).
			 */
			ret = __wt_page_in(session, page, ref, 0);
			__wt_page_release(session, page);
			WT_RET(ret);

			page = ref->page;
			slot = next ? 0 : page->entries - 1;
		}
	}
	/* NOTREACHED */
}
