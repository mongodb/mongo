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
__wt_tree_walk(SESSION *session,
    WT_PAGE *page, int (*work)(SESSION *, WT_PAGE *, void *), void *arg)
{
	BTREE *btree;
	WT_COL_REF *cref;
	WT_ROW_REF *rref;
	uint32_t i;
	int ret;

	btree = session->btree;

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL && (page = btree->root_page.page) == NULL)
		return (WT_ERROR);

	/*
	 * WT_TREE_WALK_DESCEND --
	 *	The code to descend the tree is identical for both row- and
	 * column-store pages, except for finding the WT_REF structure.
	 */
#define	WT_TREE_WALK_DESCEND(session, page, ref, work, arg) do {	\
	WT_RET(__wt_page_in(session, page, ref, 0));			\
	ret = __wt_tree_walk(session, (ref)->page, work, arg);		\
	__wt_hazard_clear(session, (ref)->page);			\
	if (ret != 0)							\
		return (ret);						\
} while (0)

	/* Walk internal pages, descending through any off-page references. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		WT_COL_REF_FOREACH(page, cref, i)
			WT_TREE_WALK_DESCEND(
			    session, page, &cref->ref, work, arg);
		break;
	case WT_PAGE_ROW_INT:
		WT_ROW_REF_FOREACH(page, rref, i)
			WT_TREE_WALK_DESCEND(
			    session, page, &rref->ref, work, arg);
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
__wt_walk_begin(SESSION *session, WT_PAGE *page, WT_WALK *walk, uint32_t flags)
{
	BTREE *btree;

	btree = session->btree;

	/*
	 * If the caller is restarting a walk, the structure may be allocated
	 * (and worse, our caller may be holding hazard references); clean up.
	 */
	if (walk->tree != NULL)
		__wt_walk_end(session, walk);

	walk->tree_len = 0;
	WT_RET(__wt_realloc(
	    session, &walk->tree_len, 20 * sizeof(WT_WALK_ENTRY), &walk->tree));
	walk->flags = flags;
	walk->tree_slot = 0;

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = btree->root_page.page;
	walk->tree[0].page = page;
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
	WT_WALK_ENTRY *e;

	/* Release any hazard references held in the stack. */
	while (walk->tree_slot > 0) {
		e = &walk->tree[--walk->tree_slot];
		if (e->hazard != NULL)
			__wt_hazard_clear(session, e->hazard);
	}

	/* Discard/reset the walk structures. */
	walk->tree_len = 0;
	__wt_free(session, walk->tree);
}

/*
 * __wt_walk_next --
 *	Return the next WT_PAGE in the tree, in a non-recursive way.
 */
int
__wt_walk_next(SESSION *session, WT_WALK *walk, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_REF *ref;
	WT_WALK_ENTRY *e;
	u_int elem;

	e = &walk->tree[walk->tree_slot];
	page = e->page;

	/* Release the hazard reference on the last page returned. */
	if (e->hazard != NULL) {
		__wt_hazard_clear(session, e->hazard);
		e->hazard = NULL;
	}

	/*
	 * Coming into this function we should have a tree internal page and
	 * we're walking the array of children.  If the status of the page is
	 * not in-memory, the tree is empty, there's nothing to do.
	 *
	 * If we've reached the end of this page, and haven't yet returned it,
	 * do that now.
	 *
	 * If the page has been returned, traversal is finished: release our
	 * hazard reference at this level, pop the stack and call ourselves
	 * recursively (unless the entire tree has been traversed, in which
	 * case, return NULL).
	 */
	if (e->visited) {
		if (walk->tree_slot == 0) {
			*pagep = NULL;
			return (0);
		}

		--walk->tree_slot;
		return (__wt_walk_next(session, walk, pagep));
	}

	if (page == NULL || e->indx == page->entries) {
eop:		e->visited = 1;
		*pagep = e->page;
		return (0);
	}

	/*
	 * Walk (or continue to talk) the internal page, returning leaf child
	 * pages, and traversing internal child pages.
	 *
	 * Eviction walks only the in-memory pages without acquiring hazard
	 * references; other callers walk the entire tree and acquire hazard
	 * references.
	 */
	if (F_ISSET(walk, WT_WALK_CACHE))
		switch (page->type) {
		case WT_PAGE_COL_INT:
			/* Find the next subtree present in the cache. */
			for (;;) {
				ref = &page->u.col_int.t[e->indx].ref;
				if (ref->state == WT_REF_MEM)
					break;
				/*
				 * If we don't find another WT_REF entry, do the
				 * post-order visit.
				 */
				if (++e->indx == page->entries)
					goto eop;
			}
			break;
		case WT_PAGE_ROW_INT:
			/* Find the next subtree present in the cache. */
			for (;;) {
				ref = &page->u.row_int.t[e->indx].ref;
				if (ref->state == WT_REF_MEM)
					break;
				/*
				 * If we don't find another WT_REF entry, do the
				 * post-order visit.
				 */
				if (++e->indx == page->entries)
					goto eop;
			}
			break;
		WT_ILLEGAL_FORMAT(session);
		}
	else {
		switch (page->type) {
		case WT_PAGE_COL_INT:
			ref = &page->u.col_int.t[e->indx].ref;
			WT_RET(__wt_page_in(session, page, ref, 0));
			break;
		case WT_PAGE_ROW_INT:
			ref = &page->u.row_int.t[e->indx].ref;
			WT_RET(__wt_page_in(session, page, ref, 0));
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		/* We just picked up a hazard reference -- save it. */
		e->hazard = ref->page;
	}

	/* Move past this page. */
	++e->indx;

	switch (ref->page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * Check to see if we grew past the end of our stack, then push
		 * the child onto the stack and recursively descend the tree.
		 */
		elem = (u_int)(walk->tree_len / WT_SIZEOF32(WT_WALK_ENTRY));
		if (walk->tree_slot + 1 >= elem)
			WT_RET(__wt_realloc(session, &walk->tree_len,
			    (elem + 20) * sizeof(WT_WALK_ENTRY), &walk->tree));
		/*
		 * Don't increment our slot until we have the memory: if the
		 * allocation fails and our caller doesn't handle the error
		 * reasonably, we don't want to be pointing off into space.
		 */
		e = &walk->tree[++walk->tree_slot];
		e->page = ref->page;
		e->hazard = NULL;
		e->indx = 0;
		e->visited = 0;
		return (__wt_walk_next(session, walk, pagep));
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		/* Return the page, it doesn't require further traversal. */
		*pagep = ref->page;
		return (0);
	WT_ILLEGAL_FORMAT(session);
	}
	/* NOTREACHED */
}
