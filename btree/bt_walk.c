/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * There are two tree-walk implementations: a textbook, depth-first recursive
 * tree walk in __wt_bt_tree_walk(), and a non-recursive, depth-first tree walk
 * in __wt_bt_walk_{begin,end,next}().
 *
 * The simple recursive walk is sufficient in most cases -- a hazard reference
 * is obtained on each page in turn, a worker function is called on the page,
 * then the hazard reference is released.
 *
 * The complicated tree walk routine was added because the cache eviction code
 * needs:
 *    + to walk the tree a few pages at a time, that is, periodically wake,
 *	visit some pages, then go back to sleep, which requires enough state
 *	to restart the traversal at any point,
 *    + to only visit pages that currently appear in the cache,
 *    + to return the WT_REF structure (not the WT_PAGE structure),
 *    + to walk files not associated with the current WT_TOC's DB handle,
 *    + and finally, it doesn't require a hazard reference.
 *
 * My guess is we'll generalize a more complicated walk at some point, which
 * means some or all of those behaviors will become configurable, and that's
 * why the code lives here instead of in the eviction code.
 */

/*
 * __wt_bt_walk_tree --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_bt_tree_walk(WT_TOC *toc, WT_REF *ref,
    uint32_t flags, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg)
{
	IDB *idb;
	WT_COL *cip;
	WT_OFF *off;
	WT_PAGE *page;
	WT_ROW *rip;
	uint32_t i;
	int ret;

	 WT_ENV_FCHK_ASSERT(
	     toc->env, "__wt_bt_tree_walk", flags, WT_APIMASK_BT_TREE_WALK);

	idb = toc->db->idb;

	/*
	 * A NULL WT_REF means to start at the top of the tree -- it's just
	 * a convenience.
	 */
	page = ref == NULL ? idb->root_page.page : ref->page;

	/*
	 * Walk any internal pages, descending through any off-page references.
	 *
	 * Descending into row-store off-page duplicate trees is optional for
	 * two reasons. (1) it may be faster to call this function recursively
	 * from the worker function, which is already walking the page, and (2)
	 * information for off-page dup trees is split (the key is on the
	 * row-leaf page, and the data is obviously in the off-page dup tree):
	 * we need the key when we dump the data, and that would be a hard
	 * special case in this code.  Functions where it's both possible and
	 * no slower to walk off-page dupliate trees in this code can request
	 * it be done here.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, cip, i) {
			/* cip references the subtree containing the record */
			ref = WT_COL_REF(page, cip);
			if (LF_ISSET(WT_WALK_CACHE) && ref->state != WT_OK)
				continue;

			off = WT_COL_OFF(cip);
			WT_RET(__wt_bt_page_in(toc, ref, off, 0));
			ret = __wt_bt_tree_walk(toc, ref, flags, work, arg);
			__wt_hazard_clear(toc, ref->page);
			if (ret != 0)
				return (ret);
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i) {
			/* rip references the subtree containing the record */
			ref = WT_ROW_REF(page, rip);
			if (LF_ISSET(WT_WALK_CACHE) && ref->state != WT_OK)
				continue;

			off = WT_ROW_OFF(rip);
			WT_RET(__wt_bt_page_in(toc, ref, off, 0));
			ret = __wt_bt_tree_walk(toc, ref, flags, work, arg);
			__wt_hazard_clear(toc, ref->page);
			if (ret != 0)
				return (ret);
		}
		break;
	case WT_PAGE_ROW_LEAF:
		if (!LF_ISSET(WT_WALK_OFFDUP))
			break;
		WT_INDX_FOREACH(page, rip, i) {
			if (WT_ITEM_TYPE(rip->data) != WT_ITEM_OFF)
				break;

			/*
			 * Recursively call the tree-walk function for the
			 * off-page duplicate tree.
			 */
			ref = WT_ROW_REF(page, rip);
			if (LF_ISSET(WT_WALK_CACHE) && ref->state != WT_OK)
				continue;

			off = WT_ROW_OFF(rip);
			WT_RET(__wt_bt_page_in(toc, ref, off, 0));
			ret = __wt_bt_tree_walk(toc, ref, flags, work, arg);
			__wt_hazard_clear(toc, ref->page);
			if (ret != 0)
				return (ret);
		}
		break;
	default:
		break;
	}

	/*
	 * Don't call the worker function for any page until all of its children
	 * have been visited.   This allows the walker function to be used for
	 * the sync method, where reconciling a modified child page modifies the
	 * parent.
	 */
	WT_RET(work(toc, page, arg));

	return (0);
}
