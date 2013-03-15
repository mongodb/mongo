/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WT_VISIT_STATE --
 *	The state maintained across calls to the "visit" callback functions:
 *	the number of refs visited, the maximum depth, and the current page and
 *	reference when moving reference into the new tree.
 */
typedef struct {
	WT_PAGE *first, *page, *second;		/* New pages to be populated. */
	WT_REF *ref, *second_ref;		/* Insert and split point. */

	uint64_t refcnt, split;			/* Ref count, split point. */
	uint64_t first_live, last_live;		/* First/last in-memory ref. */
	u_int maxdepth;				/* Maximum subtree depth. */
	int seen_live;				/* Has a ref been live? */
} WT_VISIT_STATE;

/*
 * __merge_walk --
 *	Visit all of the child references in a locked subtree and apply a
 *	callback function to them.
 */
static int
__merge_walk(WT_SESSION_IMPL *session, WT_PAGE *page, u_int depth,
	void (*visit)(WT_REF *, WT_VISIT_STATE *),
	WT_VISIT_STATE *state)
{
	WT_PAGE *child;
	WT_REF *ref;
	uint32_t i;

	if (depth > state->maxdepth)
		state->maxdepth = depth;

	WT_REF_FOREACH(page, ref, i)
		switch (ref->state) {
		case WT_REF_LOCKED:
			child = ref->page;

			/*
			 * Visit internal pages recursively.  This must match
			 * the walk in __rec_review: if the merge succeeds, we
			 * have to unlock everything.
			 */
			if (child->type == page->type &&
			    __wt_btree_mergeable(child)) {
				WT_RET(__merge_walk(
				    session, child, depth + 1, visit, state));
				break;
			}
			/* FALLTHROUGH */

		case WT_REF_DELETED:
		case WT_REF_DISK:
			(*visit)(ref, state);
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
 * __merge_count --
 *	A callback function that counts the number of references as well as
 *	the first/last "live" reference.
 */
static void
__merge_count(WT_REF *ref, WT_VISIT_STATE *state)
{
	if (ref->state == WT_REF_LOCKED) {
		if (!state->seen_live) {
			state->first_live = state->refcnt;
			state->seen_live = 1;
		}
		state->last_live = state->refcnt;
	}

	/*
	 * Sanity check that we don't overflow the counts.  We can't put more
	 * than 2**32 keys on one page anyway.
	 */
	++state->refcnt;
}

/*
 * __merge_copy_ref --
 *	Copy a child reference from the locked subtree to a new page.
 */
static void
__merge_copy_ref(WT_REF *ref, WT_VISIT_STATE *state)
{
	WT_REF *newref;

	if (state->split != 0 && state->refcnt++ == state->split)
		state->ref = state->second_ref;

	newref = state->ref++;
	*newref = *ref;
}

/*
 * __merge_unlock --
 *	Unlock all pages under an internal page being merged.
 */
static void
__merge_unlock(WT_PAGE *page)
{
	WT_REF *ref;
	uint32_t i;

	WT_REF_FOREACH(page, ref, i)
		if (ref->state == WT_REF_LOCKED) {
			if (ref->page->type == WT_PAGE_ROW_INT ||
			    ref->page->type == WT_PAGE_COL_INT)
				__merge_unlock(ref->page);
			WT_PUBLISH(ref->state, WT_REF_MEM);
		}
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __merge_check_discard --
 *	Make sure we are only discarding split-merge pages.
 */
static void
__merge_check_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *ref;
	uint32_t i;

	WT_ASSERT(session, page->type == WT_PAGE_ROW_INT ||
	    page->type == WT_PAGE_COL_INT);
	WT_ASSERT(session, page->modify != NULL &&
	    F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE));

	WT_REF_FOREACH(page, ref, i) {
		if (ref->state == WT_REF_DISK ||
		    ref->state == WT_REF_DELETED)
			continue;

		WT_ASSERT(session, ref->state == WT_REF_LOCKED);
		__merge_check_discard(session, ref->page);
	}
}
#endif

/*
 * __merge_switch_page --
 *	Switch a page from the locked tree into the new tree.
 */
static void
__merge_switch_page(WT_REF *ref, WT_VISIT_STATE *state)
{
	WT_PAGE *child;
	WT_PAGE_MODIFY *modify;
	WT_REF *newref;

	if (state->split != 0 && state->refcnt++ == state->split) {
		state->page = state->second;
		state->ref = state->second_ref;
	}

	newref = state->ref++;

	if (ref->state == WT_REF_LOCKED) {
		child = ref->page;

		/*
		 * If the child has been split, update the split page to point
		 * into the new tree.  That way, if the split-merge page is
		 * later swapped into place, it will point to the new parent.
		 *
		 * The order here is important: the parent page should point to
		 * the original child page, so we link that in last.
		 */
		if ((modify = child->modify) != NULL &&
		    F_ISSET(modify, WT_PM_REC_SPLIT))
			WT_LINK_PAGE(state->page, newref, modify->u.split);

		WT_LINK_PAGE(state->page, newref, child);

		/*
		 * If we have a child that is a live internal page, its subtree
		 * was locked by __rec_review.  We're swapping it into the new
		 * tree, unlock it now.
		 */
		if (child->type == WT_PAGE_ROW_INT ||
		    child->type == WT_PAGE_COL_INT)
			__merge_unlock(child);

		newref->state = WT_REF_MEM;
	}

	WT_CLEAR(*ref);
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

	/* Allocate a new internal page and fill it in. */
	WT_RET(__wt_page_alloc(session, type, entries, &newpage));
	newpage->read_gen = WT_READ_GEN_NOTSET;
	newpage->entries = entries;

	WT_ERR(__wt_page_modify_init(session, newpage));
	if (merge)
		F_SET(newpage->modify, WT_PM_REC_SPLIT_MERGE);
	else
		__wt_page_modify_set(session, newpage);

	*pagep = newpage;
	return (0);

err:	__wt_page_out(session, &newpage);
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
		return (__wt_row_ikey_incr(session,
		    page, 0, WT_IKEY_DATA(ikey), ikey->size, &ref->u.key));

	WT_ILLEGAL_VALUE(session);
	}
}

/*
 * __wt_merge_tree --
 *	Attempt to collapse a stack of split-merge pages in memory into a
 *	shallow tree.  If enough keys are found, create a real internal node
 *	that can be evicted (and, if necessary, split further).
 *
 *	This code is designed to deal with workloads that otherwise create
 *	arbitrarily deep (and slow) trees in memory.
 */
int
__wt_merge_tree(WT_SESSION_IMPL *session, WT_PAGE *top)
{
	WT_DECL_RET;
	WT_PAGE *lchild, *newtop, *rchild;
	WT_REF *newref;
	WT_VISIT_STATE visit_state;
	uint32_t refcnt, split;
	int promote;
	u_int levels;
	uint8_t page_type;

	WT_CLEAR(visit_state);
	lchild = newtop = rchild = NULL;
	page_type = top->type;

	WT_ASSERT(session, __wt_btree_mergeable(top));
	WT_ASSERT(session, top->ref->state == WT_REF_LOCKED);

	/*
	 * Walk the subtree, count the references at the bottom level and
	 * calculate the maximum depth.
	 */
	WT_RET(__merge_walk(session, top, 1, __merge_count, &visit_state));

	/* If there aren't enough useful levels, give up. */
	if (visit_state.maxdepth < WT_MERGE_STACK_MIN)
		return (EBUSY);

	/* Pages cannot grow larger than 2**32, but that should never happen. */
	if (visit_state.refcnt > UINT32_MAX)
		return (ENOMEM);

	/* Make sure the top page isn't queued for eviction. */
	__wt_evict_list_clr_page(session, top);

	/* Clear the eviction walk: it may be in our subtree. */
	__wt_evict_clear_tree_walk(session, NULL);

	/*
	 * Now we either collapse the internal pages into one split-merge page,
	 * or if there are "enough" keys, we split into two equal internal
	 * pages, each of which can be evicted independently.
	 *
	 * We set a flag (WT_PM_REC_SPLIT_MERGE) on the created page if it
	 * isn't big enough to justify the cost of evicting it.  If splits
	 * continue, it will be merged again until it gets over this limit.
	 */
	promote = 0;
	refcnt = (uint32_t)visit_state.refcnt;
	if (refcnt >= WT_MERGE_FULL_PAGE) {
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

		if (visit_state.first_live == visit_state.last_live &&
		    (visit_state.first_live == 0 ||
		    visit_state.first_live == refcnt - 1))
			split = (visit_state.first_live == 0) ? 1 : refcnt - 1;
		else
			split = (refcnt + 1) / 2;

		/* Only promote if we can create a real page. */
		if (split == 1 || split == refcnt - 1)
			promote = 1;
		else if (split >= WT_MERGE_FULL_PAGE &&
		    visit_state.first_live >= split)
			promote = 1;
		else if (refcnt - split >= WT_MERGE_FULL_PAGE &&
		    visit_state.last_live < split)
			promote = 1;
	}

	if (promote) {
		/* Create a new top-level split-merge page with two entries. */
		WT_ERR(__merge_new_page(session, page_type, 2, 1, &newtop));

		visit_state.split = split;

		/* Left split. */
		if (split == 1)
			visit_state.first = newtop;
		else {
			WT_ERR(__merge_new_page(session, page_type, split,
			    visit_state.first_live < split, &lchild));
			visit_state.first = lchild;
		}

		/* Right split. */
		if (split == refcnt - 1) {
			visit_state.second = newtop;
			visit_state.second_ref = &newtop->u.intl.t[1];
		} else {
			WT_ERR(__merge_new_page(session, page_type,
			    refcnt - split, visit_state.last_live >= split,
			    &rchild));
			visit_state.second = rchild;
			visit_state.second_ref =
			    &visit_state.second->u.intl.t[0];
		}
	} else {
		/*
		 * Create a new split-merge page for small merges, or if the
		 * page above is a split merge page.  When we do a big enough
		 * merge, we create a real page at the top and don't consider
		 * it as a merge candidate again.  Over time with an insert
		 * workload the tree will grow deeper, but that's inevitable,
		 * and this keeps individual merges small.
		 */
		WT_ERR(__merge_new_page(session, page_type, refcnt,
		    refcnt < WT_MERGE_FULL_PAGE ||
		    __wt_btree_mergeable(top->parent),
		    &newtop));

		visit_state.first = newtop;
	}

	/*
	 * Copy the references into the new tree, but don't update anything in
	 * the locked tree in case there is an error and we need to back out.
	 * We do this in a separate pass so that we can figure out the key for
	 * the split point: that allocates memory and so it could still fail.
	 */
	visit_state.page = visit_state.first;
	visit_state.ref = visit_state.page->u.intl.t;
	visit_state.refcnt = 0;
	WT_ERR(__merge_walk(session, top, 0, __merge_copy_ref, &visit_state));

	if (promote) {
		/* Promote keys into the top-level page. */
		if (lchild != NULL) {
			newref = &newtop->u.intl.t[0];
			WT_LINK_PAGE(newtop, newref, lchild);
			newref->state = WT_REF_MEM;
			WT_ERR(__merge_promote_key(session, newref));
		}

		if (rchild != NULL) {
			newref = &newtop->u.intl.t[1];
			WT_LINK_PAGE(newtop, newref, rchild);
			newref->state = WT_REF_MEM;
			WT_ERR(__merge_promote_key(session, newref));
		}
	}

	/*
	 * We have copied everything into place and allocated all of the memory
	 * we need.  Now link all pages into the new tree and unlock them.
	 *
	 * The only way this could fail is if a reference state has been
	 * changed by another thread since they were locked.  Panic in that
	 * case: that should never happen.
	 */
	visit_state.page = visit_state.first;
	visit_state.ref = visit_state.page->u.intl.t;
	visit_state.refcnt = 0;
	ret = __merge_walk(session, top, 0, __merge_switch_page, &visit_state);

	if (ret != 0)
		WT_ERR(__wt_illegal_value(session, "__wt_merge_tree"));

	newtop->u.intl.recno = top->u.intl.recno;
	newtop->parent = top->parent;
	newtop->ref = top->ref;

#ifdef HAVE_DIAGNOSTIC
	/*
	 * Before swapping in the new page, make sure we're only discarding
	 * split-merge pages.
	 */
	__merge_check_discard(session, top);
#endif
	/*
	 * Set up the new top-level page as a split so that it will be swapped
	 * into place by our caller.
	 */
	top->modify->flags = WT_PM_REC_SPLIT;
	top->modify->u.split = newtop;

	WT_VERBOSE_ERR(session, evict,
	    "Successfully %s %" PRIu32
	    " split-merge pages containing %" PRIu32 " keys\n",
	    promote ? "promoted" : "merged", visit_state.maxdepth, refcnt);

	/* Queue new child pages for forced eviction, if possible. */
	if (lchild != NULL && !F_ISSET(lchild->modify, WT_PM_REC_SPLIT_MERGE))
		__wt_evict_forced_page(session, lchild);
	if (rchild != NULL && !F_ISSET(rchild->modify, WT_PM_REC_SPLIT_MERGE))
		__wt_evict_forced_page(session, rchild);

	/* Update statistics. */
	WT_CSTAT_INCR(session, cache_eviction_merge);
	WT_DSTAT_INCR(session, cache_eviction_merge);

	/* How many levels did we remove? */
	levels = visit_state.maxdepth - (promote ? 2 : 1);
	WT_CSTAT_INCRV(session, cache_eviction_merge_levels, levels);
	WT_DSTAT_INCRV(session, cache_eviction_merge_levels, levels);

	return (0);

err:	WT_VERBOSE_TRET(session, evict,
	    "Failed to merge %" PRIu32
	    " split-merge pages containing %" PRIu32 " keys\n",
	    visit_state.maxdepth, refcnt);

	WT_CSTAT_INCR(session, cache_eviction_merge_fail);
	WT_DSTAT_INCR(session, cache_eviction_merge_fail);

	if (newtop != NULL)
		__wt_page_out(session, &newtop);
	if (lchild != NULL)
		__wt_page_out(session, &lchild);
	if (rchild != NULL)
		__wt_page_out(session, &rchild);
	return (ret);
}
