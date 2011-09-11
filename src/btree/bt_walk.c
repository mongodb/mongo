/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * The tree-walk implementation is a non-recursive, forward and backward,
 * depth-first tree walk in __wt_walk_{first,last,end,next,prev}().  These
 * walk routines were added to support complex walks of the tree (for example,
 * the eviction thread must be able to pause and then restart the traversal
 * at any point).
 */

/*
 * __wt_walk_init --
 *	Initialize structures for a tree walk.
 */
static int
__wt_walk_init(WT_SESSION_IMPL *session, WT_WALK *walk, uint32_t flags)
{
	/*
	 * If the caller is restarting a walk, the structure may be allocated
	 * (and worse, our caller may be holding hazard references); clean up.
	 */
	if (walk->tree != NULL)
		__wt_walk_end(session, walk, 0);

	if (walk->tree_len == 0)
		WT_RET(__wt_realloc(session,
		    &walk->tree_len, 20 * sizeof(WT_WALK_ENTRY), &walk->tree));
	walk->flags = flags;
	walk->tree_slot = 0;
	return (0);
}

/*
 * __wt_walk_first --
 *	Start a tree walk, from start to finish.
 */
int
__wt_walk_first(WT_SESSION_IMPL *session, WT_WALK *walk, uint32_t flags)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = session->btree;

	WT_RET(__wt_walk_init(session, walk, flags));

	walk->tree[0].child = 0;
	walk->tree[0].indx = 0;

	/*
	 * If there is no root page, the first call to __wt_walk_next/prev
	 * will return a NULL page.
	 */
	walk->tree[0].visited = ((page = btree->root_page.page) == NULL);
	walk->tree[0].page = page;
	return (0);
}

/*
 * __wt_walk_end --
 *	End a tree walk.
 */
void
__wt_walk_end(WT_SESSION_IMPL *session, WT_WALK *walk, int discard_walk)
{
	WT_WALK_ENTRY *e;

	if (walk->tree == NULL)
		return;

	/* Release any hazard references held in the stack. */
	for (;;) {
		e = &walk->tree[walk->tree_slot];
		if (e->hazard != NULL)
			__wt_hazard_clear(session, e->hazard);
		if (walk->tree_slot == 0)
			break;
		--walk->tree_slot;
	}

	/* Optionally free the memory, else clear the walk structures. */
	if (discard_walk) {
		__wt_free(session, walk->tree);
		walk->tree_len = 0;
	} else
		memset(walk->tree, 0, walk->tree_len);
	walk->tree_slot = 0;
	walk->flags = 0;
}

/*
 * __wt_walk_next --
 *	Return the next WT_PAGE in the tree, in a non-recursive way.
 */
int
__wt_walk_next(WT_SESSION_IMPL *session, WT_WALK *walk, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_REF *ref;
	WT_WALK_ENTRY *e;
	size_t elem;

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

	/*
	 * If we have a non-internal page at this point, we're walking a tree
	 * with a single leaf page.  Sure, make it work.
	 */
	if (e->child ||
	    (page->type != WT_PAGE_COL_INT && page->type != WT_PAGE_ROW_INT)) {
eop:		e->child = e->visited = 1;
		*pagep = e->page;
		return (0);
	}

	/*
	 * Walk (or continue to walk) the internal page, returning leaf child
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
			break;
		case WT_PAGE_ROW_INT:
			ref = &page->u.row_int.t[e->indx].ref;
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		/* We just picked up a hazard reference -- save it. */
		WT_RET(__wt_page_in(session, page, ref, 0));
		e->hazard = ref->page;
	}

	/* Move past this page. */
	if (++e->indx == page->entries)
		e->child = 1;

	switch (ref->page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * Check to see if we grew past the end of our stack, then push
		 * the child onto the stack and recursively descend the tree.
		 */
		elem = walk->tree_len / sizeof(WT_WALK_ENTRY);
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
		e->child = e->visited = 0;
		return (__wt_walk_next(session, walk, pagep));
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		/* Return the page, it doesn't require further traversal. */
		*pagep = ref->page;
		return (0);
	WT_ILLEGAL_FORMAT(session);
	}
	/* NOTREACHED */
}

/*
 * __wt_walk_prev --
 *	Return the previous WT_PAGE in the tree, in a non-recursive way.
 */
int
__wt_walk_prev(WT_SESSION_IMPL *session, WT_WALK *walk, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_REF *ref;
	WT_WALK_ENTRY *e;
	size_t elem;

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
	 * If we've reached the start of this page, and haven't yet returned
	 * it, do that now.
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
		return (__wt_walk_prev(session, walk, pagep));
	}

	/*
	 * If we have a non-internal page at this point, we're walking a tree
	 * with a single leaf page.  Sure, make it work.
	 */
	if (e->child ||
	    (page->type != WT_PAGE_COL_INT && page->type != WT_PAGE_ROW_INT)) {
		e->child = e->visited = 1;
		*pagep = e->page;
		return (0);
	}

	/*
	 * Walk (or continue to walk) the internal page, returning leaf child
	 * pages, and traversing internal child pages.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		ref = &page->u.col_int.t[e->indx].ref;
		break;
	case WT_PAGE_ROW_INT:
		ref = &page->u.row_int.t[e->indx].ref;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* We just picked up a hazard reference -- save it. */
	WT_RET(__wt_page_in(session, page, ref, 0));
	e->hazard = ref->page;

	/* Move past this page. */
	if (e->indx == 0)
		e->child = 1;
	else
		--e->indx;

	switch (ref->page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * Check to see if we grew past the end of our stack, then push
		 * the child onto the stack and recursively descend the tree.
		 */
		elem = walk->tree_len / sizeof(WT_WALK_ENTRY);
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
		e->indx = ref->page->entries - 1;
		e->child = e->visited = 0;
		return (__wt_walk_next(session, walk, pagep));
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		/* Return the page, it doesn't require further traversal. */
		*pagep = ref->page;
		return (0);
	WT_ILLEGAL_FORMAT(session);
	}
	/* NOTREACHED */
}

/*
 * __wt_tree_np --
 *	Move to the next/previous page in the tree.
 */
int
__wt_tree_np(WT_SESSION_IMPL *session, WT_PAGE **pagep, int next)
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
	if ((next && slot == page->entries - 1) || (!next && slot == 0)) {
		*pagep = page;
		return (0);
	}
	if (next)
		++slot;
	else
		--slot;

descend:
	/*
	 * We're starting a new subtree on page/slot, descend to the left-most
	 * item in the subtree, swapping hazard references at each level (but
	 * don't leave a hazard reference dangling on error).
	 */
	for (;;) {
		if (page->type == WT_PAGE_ROW_INT)
			ref = &page->u.row_int.t[slot].ref;
		else if (page->type == WT_PAGE_COL_INT)
			ref = &page->u.col_int.t[slot].ref;
		else
			break;

		ret = __wt_page_in(session, page, ref, 0);
		__wt_page_release(session, page);
		WT_RET(ret);
		page = ref->page;
		slot = next ? 0 : page->entries - 1;
	}
	*pagep = page;
	return (0);
}
