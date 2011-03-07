/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * There are two tree-walk implementations: a textbook, depth-first recursive
 * tree walk in __wt_tree_walk(), and a non-recursive, depth-first tree walk
 * in __wt_walk_{begin,end,next}().
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
 *    + to walk files not associated with the current SESSION's BTREE handle,
 *    + and finally, it doesn't require a hazard reference.
 *
 * My guess is we'll generalize a more complicated walk at some point, which
 * means some or all of those behaviors will become configurable, and that's
 * why the code lives here instead of in the eviction code.
 */

/*
 * __wt_tree_walk --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_tree_walk(SESSION *session, WT_PAGE *page,
    uint32_t flags, int (*work)(SESSION *, WT_PAGE *, void *), void *arg)
{
	BTREE *btree;
	WT_COL_REF *cref;
	WT_ROW_REF *rref;
	WT_REF *ref;
	uint32_t i;
	int ret;

	 WT_CONN_FCHK(
	     S2C(session), "__wt_tree_walk", flags, WT_APIMASK_BT_TREE_WALK);

	btree = session->btree;

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = btree->root_page.page;

	/* Walk internal pages, descending through any off-page references. */
	switch (page->dsk->type) {
	case WT_PAGE_COL_INT:
		WT_COL_REF_FOREACH(page, cref, i) {
			/* cref references the subtree containing the record */
			switch (WT_COL_REF_STATE(cref)) {
			case WT_REF_CACHE:
				break;
			case WT_REF_DELETED:
				continue;
			case WT_REF_DISK:
			case WT_REF_EVICT:
				if (LF_ISSET(WT_WALK_CACHE))
					continue;
				break;
			}
			ref = &cref->ref;
			switch (ret = __wt_page_in(session, page, ref, 0)) {
			case 0:				/* Valid page */
				ret = __wt_tree_walk(
				    session, ref->page, flags, work, arg);
				__wt_hazard_clear(session, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				return (ret);
		}
		break;
	case WT_PAGE_ROW_INT:
		WT_ROW_REF_FOREACH(page, rref, i) {
			/* rref references the subtree containing the record */
			switch (WT_ROW_REF_STATE(rref)) {
			case WT_REF_CACHE:
				break;
			case WT_REF_DELETED:
				continue;
			case WT_REF_DISK:
			case WT_REF_EVICT:
				if (LF_ISSET(WT_WALK_CACHE))
					continue;
				break;
			}
			ref = &rref->ref;
			switch (ret = __wt_page_in(session, page, ref, 0)) {
			case 0:				/* Valid page */
				ret = __wt_tree_walk(
				    session, ref->page, flags, work, arg);
				__wt_hazard_clear(session, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				return (ret);
		}
		break;
	}

	/*
	 * Don't call the worker function for any page until all of its children
	 * have been visited.   This allows the walker function to be used for
	 * the close/sync methods, where reconciling a modified child page will
	 * modify its parent.
	 */
	WT_RET(work(session, page, arg));

	return (0);
}

/*
 * __wt_walk_begin --
 *	Start a tree walk.
 */
int
__wt_walk_begin(SESSION *session, WT_REF *ref, WT_WALK *walk)
{
	/*
	 * The caller may be restarting a walk, so the structure may already
	 * be allocated.  Allocate 20 slots: it's always going to be enough.
	 */
	if (walk->tree_len == 0)
		WT_RET(__wt_realloc(session, &walk->tree_len,
		    20 * sizeof(WT_WALK_ENTRY), &walk->tree));
	walk->tree_slot = 0;

	walk->tree[0].ref = ref;
	walk->tree[0].indx = 0;
	walk->tree[0].visited = 0;

	return (0);
}

/*
 * __wt_walk_end --
 *	End a tree walk.
 */
void
__wt_walk_end(SESSION *session, WT_WALK *walk)
{
	__wt_free(session, walk->tree, walk->tree_len);
}

/*
 * __wt_walk_next --
 *	Return the next WT_REF/WT_PAGE in the tree, in a non-recursive way.
 */
int
__wt_walk_next(SESSION *session, WT_WALK *walk, uint32_t flags, WT_REF **refp)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_REF *ref;
	WT_ROW_REF *rref;
	WT_WALK_ENTRY *e;
	u_int elem;
	int ret;

	e = &walk->tree[walk->tree_slot];
	page = e->ref->page;

	/*
	 * Coming into this function we have a tree internal page and we're
	 * walking the array of children.
	 *
	 * If we've reached the end of this page, and haven't yet returned it,
	 * do that now.  If the page has been returned, traversal is finished:
	 * pop the stack and call ourselve recursively, unless the entire tree
	 * has been traversed, in which case we return NULL.
	 */
	if (e->visited) {
		if (walk->tree_slot == 0) {
			*refp = NULL;
			return (0);
		} else {
			--walk->tree_slot;
			return (__wt_walk_next(session, walk, flags, refp));
		}
	} else
		if (e->indx == page->indx_count) {
eop:			e->visited = 1;
			*refp = e->ref;
			return (0);
		}

	/*
	 * Check to see if the page has sub-trees associated with it, in which
	 * case we traverse those pages.
	 */
	switch (page->dsk->type) {
	case WT_PAGE_COL_INT:
		/* Find the next subtree present in the cache. */
		for (;;) {
			cref = &page->u.col_int.t[e->indx];
			ref = &cref->ref;

			/* We only care about pages in the cache. */
			if (ref->state == WT_REF_CACHE)
				break;
			else if (ref->state != WT_REF_DELETED &&
			    !LF_ISSET(WT_WALK_CACHE)) {
				ret = __wt_page_in(session, page, ref, 0);
				if (ret == 0)
					break;			/* Valid page */
				else if (ret == WT_PAGE_DELETED)
					ret = 0;
				else
					return (ret);
			}

			/*
			 * If we don't find another WT_REF entry, do the
			 * post-order visit.
			 */
			if (++e->indx == page->indx_count)
				goto eop;
		}
		break;
	case WT_PAGE_ROW_INT:
		/* Find the next subtree present in the cache. */
		for (;;) {
			rref = &page->u.row_int.t[e->indx];
			ref = &rref->ref;

			/* We only care about pages in the cache. */
			if (ref->state == WT_REF_CACHE)
				break;
			else if (ref->state != WT_REF_DELETED &&
			    !LF_ISSET(WT_WALK_CACHE)) {
				ret = __wt_page_in(session, page, ref, 0);
				if (ret == 0)
					break;			/* Valid page */
				else if (ret == WT_PAGE_DELETED)
					ret = 0;
				else
					return (ret);
			}

			/*
			 * If we don't find another WT_REF entry, do the
			 * post-order visit.
			 */
			if (++e->indx == page->indx_count)
				goto eop;
		}
		break;
	}

	switch (ref->page->dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* The page has children; first, move past this child. */
		++e->indx;

		/*
		 * Check to see if we grew past the end of our stack, then push
		 * the child onto the stack and recursively descend the tree.
		 */
		elem = (u_int)(walk->tree_len / WT_SIZEOF32(WT_WALK_ENTRY));
		if (walk->tree_slot >= elem)
			WT_RET(__wt_realloc(session, &walk->tree_len,
			    (elem + 20) * sizeof(WT_WALK_ENTRY), &walk->tree));

		e = &walk->tree[++walk->tree_slot];
		e->ref = ref;
		e->indx = 0;
		e->visited = 0;
		return (__wt_walk_next(session, walk, flags, refp));
	}

	/* Return the child page, it's not interesting for further traversal. */
	++e->indx;
	*refp = ref;
	return (0);
}
