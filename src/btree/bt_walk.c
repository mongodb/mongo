/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * There are two tree-walk implementations: a textbook, depth-first recursive
 * tree walk in __wt_tree_walk(), and a non-recursive, forward and backward,
 * depth-first tree walk in __wt_walk_{first,last,end,next,prev}().
 *
 * The simple recursive walk is sufficient in most cases: a hazard reference
 * is obtained on each page in turn, a worker function is called on the page,
 * then the hazard reference is released.
 *
 * The complicated tree walk routine was added to support complex walks of the
 * tree (for example, cursors need to walk the tree in reverse order, and both
 * the eviction thread and cursors must be able to pause and then restart the
 * traversal at any point).
 */

/*
 * __wt_tree_walk --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session,
    WT_PAGE *page, int (*work)(WT_SESSION_IMPL *, WT_PAGE *, void *), void *arg)
{
	WT_BTREE *btree;
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
	 * Call the worker function for a page after all of its children have
	 * been visited; nothing depends on this semantic, but if some future
	 * operation wants to operate on a parent/child combination, this is
	 * the right approach.
	 */
	return (work(session, page, arg));
}

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

	if ((page = btree->root_page.page) == NULL)
		return (WT_ERROR);
	walk->tree[0].page = page;
	walk->tree[0].indx = 0;
	walk->tree[0].child = walk->tree[0].visited = 0;
	return (0);
}

/*
 * __wt_walk_last --
 *	Start a tree walk, from finish to start.
 */
int
__wt_walk_last(WT_SESSION_IMPL *session, WT_WALK *walk, uint32_t flags)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_REF *ref;
	WT_WALK_ENTRY *e;
	u_int elem;

	btree = session->btree;

	WT_RET(__wt_walk_init(session, walk, flags));

	if ((page = btree->root_page.page) == NULL)
		return (WT_ERROR);
	walk->tree[0].page = page;
	walk->tree[0].indx = page->entries - 1;
	walk->tree[0].child = walk->tree[0].visited = 0;

	/* Move to the last entry on the last page in the tree. */
	for (e = &walk->tree[0];;) {
		page = e->page;
		switch (page->type) {
		case WT_PAGE_COL_INT:
			ref = &page->u.col_int.t[page->entries - 1].ref;
			break;
		case WT_PAGE_ROW_INT:
			ref = &page->u.row_int.t[page->entries - 1].ref;
			break;
		default:
			return (0);
		}

		WT_RET(__wt_page_in(session, page, ref, 0));
		e->hazard = ref->page;

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
		e->indx = ref->page->entries - 1;
		e->child = e->visited = 0;
	}
	/* NOTREACHED */
}

/*
 * __wt_walk_set --
 *	Start a tree walk, from an existing location.
 */
int
__wt_walk_set(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_WALK *walk, uint32_t flags)
{
	WT_PAGE *t;
	WT_REF *ref;
	WT_ROW_REF *rref;
	WT_WALK_ENTRY *e;
	uint32_t elem, i, levels;

	/*
	 * Figure out how deep we are in the tree, and allocate sufficient
	 * slots in the walk structure.
	 */
	for (t = page->parent, levels = 1; t != NULL; t = t->parent, ++levels)
		;
	elem = (u_int)(walk->tree_len / WT_SIZEOF32(WT_WALK_ENTRY));
	if (levels > elem)
		WT_RET(__wt_realloc(session, &walk->tree_len,
		    (levels + 10) * sizeof(WT_WALK_ENTRY), &walk->tree));
	walk->flags = flags;

	/*
	 * Walk back up the tree, acquiring hazard references and filling in
	 * slots as we go.  Set the number of slots immediately, that way we
	 * can simply return on error, and "ending" the walk will clean up.
	 *
	 * We only need an entry for each internal tree level, correct for a
	 * leaf page, and for 0-based references.
	 */
	walk->tree_slot = levels - 2;
	e = &walk->tree[walk->tree_slot];

	/*
	 * The hazard entry at each slot is the NEXT page, that is, it's the
	 * hazard reference for the page in the next slot, and, at the leaf
	 * level, the leaf page being returned.
	 */
	e->hazard = page;

	/* Walk the tree, filling in slots. */
	for (t = page->parent, ref = page->parent_ref;;
	    ref = t->parent_ref, t = t->parent) {
		/*
		 * Acquire a hazard reference on every page but the root (pages
		 * can't be discarded as long as they have children, but if the
		 * leaf page is discarded, and nobody has a hazard reference on
		 * its internal parent page, it could theoretically be discarded
		 * while still in use by a cursor).  However, we do not want a
		 * hazard reference on the root page, the root page is pinned.
		 */
		if (t->parent != NULL)
			WT_RET(__wt_page_in(session, t, ref, 0));
		e->page = t;
		e->child = e->visited = 0;

		switch (t->type) {
		case WT_PAGE_COL_INT:
			break;
		case WT_PAGE_ROW_INT:
			WT_ROW_REF_FOREACH(t, rref, i)
				if (&rref->ref == ref) {
					e->indx = WT_ROW_REF_SLOT(t, rref) + 1;
					if (e->indx == t->entries)
						e->child = 1;
					break;
				}
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		if (t->parent == NULL)
			break;
		--e;
		e->hazard = t;
	}

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
	WT_ROW_REF *rref;
	uint32_t slot;

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
			return (WT_ERROR);
		slot = 0;
		goto descend;
	}

	/* If the active page was the root, we've reached the walk's end. */
	if (WT_PAGE_IS_ROOT(page))
		return (0);

	/*
	 * Swap our hazard reference for the hazard reference of our parent.
	 *
	 * We're hazard-reference coupling up the tree and that's OK: first,
	 * hazard references can't deadlock, so there's none of the usual
	 * problems found when logically locking up a Btree; second, we don't
	 * release our current hazard reference until we have our parent's
	 * hazard reference.  If the eviction thread tries to evict the active
	 * page, that fails because of our hazard reference.  If eviction tries
	 * to evict our parent, that fails because the parent has a child page
	 * that can't be discarded.
	 *
	 * Get the page if it's not the root page; we could access it directly
	 * because we know it's in memory, but we need a hazard reference.
	 */
	t = page->parent;
	if (!WT_PAGE_IS_ROOT(t))
		WT_RET(__wt_page_in(session, t, t->parent_ref, 0));

	/* Figure out the currently slot. */
	slot = WT_ROW_REF_SLOT(t, page->parent_ref);

	/* Release our previous hazard reference. */
	__wt_page_release(session, page);
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
	 * item in the subtree, swapping hazard references at each level.
	 */
	for (;;) {
		rref = &page->u.row_int.t[slot];
		WT_RET(__wt_page_in(session, page, &rref->ref, 0));
		__wt_page_release(session, page);

		page = WT_ROW_REF_PAGE(rref);
		if (page->type != WT_PAGE_ROW_INT)
			break;
		slot = next ? 0 : page->entries - 1;
	}
	*pagep = page;
	return (0);
}
