/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __merge_walk --
 *	Visit all of the child references in a locked subtree and apply a
 *	callback function to them.
 */
static int
__merge_walk(WT_SESSION_IMPL *session, WT_PAGE *page, u_int depth,
	int (*visit)(WT_REF *, u_int, void *), void *cookie)
{
	WT_PAGE *child;
	WT_REF *ref;
	uint32_t i;

	WT_REF_FOREACH(page, ref, i)
		switch (ref->state) {
		case WT_REF_LOCKED:
			/*
			 * Don't allow eviction until it is properly hooked
			 * into the tree.
			 */
			__wt_evict_list_clr_page(session, ref->page);

			child = ref->page;
			/* Visit internal pages recursively. */
			if (child->type == page->type &&
			    child->modify != NULL &&
			    F_ISSET(child->modify, WT_PM_REC_SPLIT_MERGE)) {
				WT_RET(__merge_walk(
				    session, child, depth + 1, visit, cookie));
				break;
			}
			/* FALLTHROUGH */

		case WT_REF_DELETED:
		case WT_REF_DISK:
			WT_RET((*visit)(ref, depth, cookie));
			break;

		case WT_REF_EVICT_FORCE:
		case WT_REF_EVICT_WALK:
		case WT_REF_MEM:
		case WT_REF_READING:
		WT_ILLEGAL_VALUE(session);
		}

	return (0);
}

/*
 * WT_VISIT_STATE --
 *	The state maintained across calls to the "visit" callback functions:
 *	the number of refs visited, the maximum depth, and the current page and
 *	reference when moving reference into the new tree.
 */
typedef struct {
	WT_PAGE *current, *lchild, *rchild;	/* New pages to be populated. */
	WT_REF *ref;				/* Current insert point. */

	uint32_t refcnt, split;			/* Ref count, split point. */
	uint32_t first_live, last_live;		/* First/last in-memory ref. */
	u_int maxdepth;				/* Maximum subtree depth. */
	int seen_live;				/* Has a ref been live? */
} WT_VISIT_STATE;

/*
 * __merge_count --
 *	A callback function that counts the number of references as well as
 *	the first/last "live" reference.
 */
static int
__merge_count(WT_REF *ref, u_int depth, void *cookie)
{
	WT_VISIT_STATE *state;

	state = cookie;

	if (depth > state->maxdepth)
		state->maxdepth = depth;
	if (ref->state == WT_REF_LOCKED) {
		if (!state->seen_live)
			state->first_live = state->refcnt;
		state->last_live = state->refcnt;
		state->seen_live = 1;
	}

	/* Sanity check that we don't have more than 2**32 refs. */
	if (++state->refcnt == 0)
		return (ENOMEM);
	return (0);
}

/*
 * __merge_move_ref --
 *	Move a child ref from the locked subtree to a new page.
 */
static int
__merge_move_ref(WT_REF *ref, u_int depth, void *cookie)
{
	WT_REF *newref;
	WT_VISIT_STATE *state;

	WT_UNUSED(depth);
	state = cookie;

	if (state->split != 0 && state->refcnt++ == state->split) {
		state->current = state->rchild;
		state->ref = &state->rchild->u.intl.t[0];
	}

	/* Move a ref from the old tree to the new tree. */
	newref = state->ref++;
	*newref = *ref;
	WT_CLEAR(*ref);
	if (newref->page != NULL)
		WT_LINK_PAGE(state->current, newref, newref->page);
	if (newref->state == WT_REF_LOCKED)
		newref->state = WT_REF_MEM;
	else
		WT_ASSERT(NULL, newref->page == NULL);

	return (0);
}

/*
 * __merge_new_page --
 *	Create a new in-memory internal page.
 */
static int
__merge_new_page(WT_SESSION_IMPL *session,
	uint8_t type, uint32_t entries, int merge, WT_PAGE **pagep)
{
	WT_DECL_RET;
	WT_PAGE *newpage;

	WT_RET(__wt_calloc_def(session, 1, &newpage));
	WT_ERR(__wt_calloc_def(session, (size_t)entries, &newpage->u.intl.t));

	/* Fill it in. */
	newpage->read_gen = __wt_cache_read_gen(session);
	newpage->entries = entries;
	newpage->type = type;

	WT_ERR(__wt_page_modify_init(session, newpage));
	if (merge)
		F_SET(newpage->modify, WT_PM_REC_SPLIT_MERGE);
	else
		__wt_page_modify_set(session, newpage);

	*pagep = newpage;
	return (0);

err:	__wt_free(session, newpage->u.intl.t);
	__wt_free(session, newpage);
	return (ret);
}

/*
 * __merge_promote_key --
 *	Copy a key from a child page into the reference in its parent, so it
 *	can be found by searches.
 */
static int
__merge_promote_key(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_IKEY *ikey;
	WT_PAGE *page;
	WT_REF *child_ref;

	page = ref->page;
	switch (page->type) {
	case WT_PAGE_COL_INT:
		child_ref = &page->u.intl.t[0];
		ref->u.recno = page->u.intl.recno = child_ref->u.recno;
		return (0);

	case WT_PAGE_ROW_INT:
		child_ref = &page->u.intl.t[0];
		ikey = child_ref->u.key;
		WT_ASSERT(session, ikey != NULL);
		return (__wt_row_ikey_alloc(
		    session, 0, WT_IKEY_DATA(ikey), ikey->size, &ref->u.key));

	WT_ILLEGAL_VALUE(session);
	}
}

/*
 * Attempt to collapse a stack of split-merge pages in memory into a shallow
 * tree.  If enough keys are found, create a real internal node that can be
 * evicted (and, if necessary, split further).
 *
 * This code is designed to deal with the case of append-only workloads that
 * otherwise create arbitrarily deep (and slow) trees in memory.
 */
int
__wt_merge_tree(WT_SESSION_IMPL *session, WT_PAGE *top)
{
	WT_DECL_RET;
	WT_PAGE *newtop;
	WT_REF *newref;
	WT_VISIT_STATE visit_state;
	uint32_t refcnt, split;
	int promote;
	uint8_t page_type;

	WT_CLEAR(visit_state);
	newtop = NULL;
	page_type = top->type;

	WT_ASSERT(session, !WT_PAGE_IS_ROOT(top));
	WT_ASSERT(session, top->ref->state == WT_REF_LOCKED);

	/* Check how big the stack of pages is. */
	WT_RET(__merge_walk(session, top, 0, __merge_count, &visit_state));

	/* If there aren't enough useful levels or refs, stop. */
	if (visit_state.maxdepth < WT_MERGE_STACK_MIN)
		return (EBUSY);

	/*
	 * Now we either collapse the internal pages into one split-merge page,
	 * or if there are "enough" keys, we split into two equal internal
	 * pages, each of which can be evicted independently.
	 *
	 * We set a flag (WT_PM_REC_SPLIT_MERGE) on the created page if it
	 * isn't big enough to justify the cost of evicting it.  If splits
	 * continue, it will be merged again until it gets over this limit.
	 */
	refcnt = visit_state.refcnt;
	promote = (refcnt > 100);

	if (promote) {
		/* Create a new top-level split-merge page with two children. */
		WT_ERR(__merge_new_page(session, page_type, 2, 1, &newtop));

		/*
		 * In the normal case where there are live children spread
		 * through the subtree, create two child pages.
		 *
		 * Handle the case where the only live child is first / last
		 * specially: put the live child into the top-level page.
		 *
		 * Set SPLIT_MERGE on the internal pages if there are any live
		 * children: they can't be evicted, so there is no point
		 * permanently deepening the tree.
		 */
		if ((split = visit_state.first_live) != visit_state.last_live ||
		    split != 0 || split != refcnt - 1)
			visit_state.split = split = (refcnt + 1) / 2;

		/* Left split. */
		if (split == 0) {
			visit_state.split = split = 1;
			visit_state.current = visit_state.lchild = newtop;
		} else {
			WT_ERR(__merge_new_page(session, page_type,
			    split, visit_state.first_live < split,
			    &visit_state.lchild));
			visit_state.current = visit_state.lchild;
		}

		/* Right split. */
		if (split == refcnt - 1) {
			visit_state.split = refcnt - 1;
			visit_state.rchild = newtop;
		} else
			WT_ERR(__merge_new_page(session, page_type,
			    refcnt - split, visit_state.last_live >= split,
			    &visit_state.rchild));
	} else {
		WT_ERR(__merge_new_page(
		    session, page_type, refcnt, 1, &newtop));

		visit_state.current = newtop;
	}

	visit_state.ref = visit_state.current->u.intl.t;
	visit_state.refcnt = 0;
	WT_ERR(__merge_walk(session, top, 0, __merge_move_ref, &visit_state));

	if (promote) {
		/* Promote keys into the top-level page. */
		newref = &newtop->u.intl.t[0];
		WT_LINK_PAGE(newtop, newref, visit_state.lchild);
		newref->state = WT_REF_MEM;
		WT_ERR(__merge_promote_key(session, newref));

		++newref;
		WT_LINK_PAGE(newtop, newref, visit_state.rchild);
		newref->state = WT_REF_MEM;
		WT_ERR(__merge_promote_key(session, newref));

		/* Queue new pages for forced eviction. */
		if (!F_ISSET(visit_state.lchild->modify, WT_PM_REC_SPLIT_MERGE))
			__wt_evict_forced_page(session, visit_state.lchild);
		if (!F_ISSET(visit_state.rchild->modify, WT_PM_REC_SPLIT_MERGE))
			__wt_evict_forced_page(session, visit_state.rchild);
	}

	newtop->u.intl.recno = top->u.intl.recno;
	newtop->parent = top->parent;
	newtop->ref = top->ref;

	/*
	 * Set up the new top-level page as a split so that it will be swapped
	 * into place by __wt_evict_page.
	 */
	top->modify->u.split = newtop;
	top->modify->flags = WT_PM_REC_SPLIT;

	WT_VERBOSE_ERR(session, evict,
	    "Successfully %s %" PRIu32
	    " split-merge pages containing %" PRIu32 " keys\n",
	    promote ? "promoted" : "merged", visit_state.maxdepth, refcnt);

	return (0);

err:
	WT_VERBOSE_TRET(session, evict,
	    "Failed to merge %" PRIu32
	    " split-merge pages containing %" PRIu32 " keys\n",
	    visit_state.maxdepth, refcnt);

	if (newtop != NULL)
		__wt_page_out(session, &newtop);
	if (visit_state.lchild != NULL)
		__wt_page_out(session, &visit_state.lchild);
	if (visit_state.rchild != NULL)
		__wt_page_out(session, &visit_state.rchild);
	return (ret);
}
