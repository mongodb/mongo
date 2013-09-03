/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __split_row_page_inmem --
 *	Split a row leaf page in memory. This is done when we are unable to
 *	evict the page due to contention. The code is aimed at append
 *	workloads. We turn a single page into three pages:
 *	 * A SPLIT_MERGE internal page with two children.
 *	 * The left child is the original page, with it's right most element
 *	   removed.
 *	 * The right child has the element removed from the original page.
 */
static int
__split_row_page_inmem(WT_SESSION_IMPL *session, WT_PAGE *orig)
{
	WT_DECL_RET;
	WT_INSERT *current_ins, *ins, **insp, *prev_ins;
	WT_INSERT_HEAD *ins_head, **new_ins_head_list, *new_ins_head;
	WT_PAGE *new_parent, *right_child;
	WT_REF *newref;
	int i, ins_depth;
	size_t transfer_size;
	void *p;
	uint32_t size;

	new_parent = right_child = NULL;
	new_ins_head_list = NULL;
	new_ins_head = NULL;
	ins = NULL;

	/*
	 * Only split a page once, otherwise workloads that update in the
	 * middle of the page could continually split without any benefit.
	 */
	if (F_ISSET_ATOMIC(orig, WT_PAGE_WAS_SPLIT))
		return (EBUSY);
	F_SET_ATOMIC(orig, WT_PAGE_WAS_SPLIT);

	/* Find the final item on the original page. */
	if (orig->entries == 0)
		ins_head = WT_ROW_INSERT_SMALLEST(orig);
	else
		ins_head = WT_ROW_INSERT_SLOT(orig, orig->entries - 1);

	ins = WT_SKIP_LAST(ins_head);

	/*
	 * The logic below requires more than one element in the skip list.
	 * There is no point splitting if the list is small (no deep items is
	 * a good heuristic for that) - it's likely that the page isn't
	 * part of an append workload.
	 */
#define	WT_MIN_SPLIT_SKIPLIST_DEPTH	WT_MIN(5, WT_SKIP_MAXDEPTH -1)
	if (ins == NULL || ins_head->head[0] == ins_head->tail[0] ||
	    ins_head->head[WT_MIN_SPLIT_SKIPLIST_DEPTH] == NULL)
		return (EBUSY);

	/*
	 * Do all operations that can fail before futzing with the original
	 * page.
	 * Allocate two new pages:
	 *  * One split merge page that will be the new parent.
	 *  * One leaf page (right_child).
	 * We will move the last key/value pair from the original page onto
	 * the new leaf page.
	 */

	WT_RET(__wt_btree_new_modified_page(
	    session, WT_PAGE_ROW_INT, 2, 1, &new_parent));

	WT_ERR(__wt_btree_new_modified_page(
	    session, WT_PAGE_ROW_LEAF, 0, 0, &right_child));

	/* Add the entry onto the right_child page, as a skip list. */
	WT_ERR(__wt_calloc_def(session, 1, &new_ins_head_list));
	WT_ERR(__wt_calloc_def(session, 1, &new_ins_head));
	right_child->u.row.ins = new_ins_head_list;
	right_child->u.row.ins[0] = new_ins_head;
	__wt_cache_page_inmem_incr(session,
	    right_child, sizeof(WT_INSERT_HEAD) + sizeof(WT_INSERT_HEAD *));

	/*
	 * Figure out how deep the skip list stack is for the element we are
	 * removing and install the element on the new child page.
	 */
	for (ins_depth = 0;
	    ins_depth < WT_SKIP_MAXDEPTH && ins_head->tail[ins_depth] == ins;
	    ins_depth++)
		;
	for (i = 0; i < ins_depth; i++) {
		right_child->u.row.ins[0]->tail[i] = ins;
		right_child->u.row.ins[0]->head[i] = ins;
	}

	/* Setup the ref in the new parent to point to the original parent. */
	new_parent->parent = orig->parent;
	new_parent->ref = orig->ref;

	/* Link the right child page into the new parent. */
	newref = &new_parent->u.intl.t[1];
	WT_LINK_PAGE(new_parent, newref, right_child);
	newref->state = WT_REF_MEM;

	/*
	 * Copy the key we moved onto the right child into the WT_REF
	 * structure that is linked into the new parent page.
	 */
	WT_ERR(__wt_row_ikey_incr(session, new_parent, 0,
	    WT_INSERT_KEY(ins), ins->u.key.size, &newref->key.ikey));

	/*
	 * Create a copy of the key for the left child in the new parent - do
	 * this first, since we copy it out of the original ref. Then update
	 * the original ref to point to the new parent
	 */
	newref = &new_parent->u.intl.t[0];
	__wt_ref_key(orig->parent, orig->ref, &p, &size);
	WT_ERR (__wt_row_ikey_incr(
	    session, new_parent, 0, p, size, &newref->key.ikey));
	WT_LINK_PAGE(new_parent, newref, orig);
	newref->state = WT_REF_MEM;

	/*
	 * Now that all operations that could fail have completed, we start
	 * updating the original page.
	 */
	transfer_size =
	    sizeof(WT_INSERT) + ((size_t)ins_depth * sizeof(WT_INSERT *)) +
	    WT_INSERT_KEY_SIZE(ins);
	__wt_cache_page_inmem_incr(session, right_child, transfer_size);
	__wt_cache_page_inmem_decr(session, orig, transfer_size);

	/*
	 * Remove the entry from the orig page (i.e truncate the skip list).
	 * Following is an example skip list that might help.
	 *
	 *               __
	 *              |c3|
	 *               |
	 *   __		 __    __
	 *  |a2|--------|c2|--|d2|
	 *   |		 |	|
	 *   __		 __    __	   __
	 *  |a1|--------|c1|--|d1|--------|f1|
	 *   |		 |	|	   |
	 *   __    __    __    __    __    __
	 *  |a0|--|b0|--|c0|--|d0|--|e0|--|f0|
	 *
	 *   From the above picture.
	 *   The head array will be: a0, a1, a2, c3, NULL
	 *   The tail array will be: f0, f1, d2, c3, NULL
	 *   We are looking for: e1, d2, NULL
	 *   If there were no f1, we'd be looking for: e0, NULL
	 *   If there were an f2, we'd be looking for: e0, d1, d2, NULL
	 *
	 *   The algorithm does:
	 *   1) Start at the top of the head list.
	 *   2) Step down until we find a level that contains more than one
	 *      element.
	 *   3) Step across until we reach the tail of the level.
	 *   4) If the tail is the item being moved, remove it.
	 *   5) Drop down a level, and go to step 3 until at level 0.
	 */
	prev_ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i];
	    i >= 0;
	    i--, insp--) {
		/* Level empty, or a single element. */
		if (ins_head->head[i] == NULL ||
		     ins_head->head[i] == ins_head->tail[i]) {
			/* Remove if it is the element being moved. */
			if (ins_head->head[i] == ins)
				ins_head->head[i] = ins_head->tail[i] = NULL;
			continue;
		}

		for (current_ins = *insp;
		    current_ins != ins_head->tail[i];
		    current_ins = current_ins->next[i])
			prev_ins = current_ins;

		/*
		 * Update the stack head so that we step down as far to the
 		 * the right as possible. We know that prev_ins is valid
 		 * since levels must contain at least two items to be here.
 		 */
		insp = &prev_ins->next[i];
		if (current_ins == ins) {
			/* Remove the item being moved. */
			WT_ASSERT(session, ins_head->head[i] != ins);
			WT_ASSERT(session, prev_ins->next[i] == ins);
			*insp = NULL;
			ins_head->tail[i] = prev_ins;
		}
	}

	/*
	 * Set up the new top-level page as a split so that it will be swapped
	 * into place by our caller.
	 */
	orig->modify->flags = WT_PM_REC_SPLIT;
	orig->modify->u.split = new_parent;

	/* Make it likely we evict the page we just split. */
	orig->read_gen = WT_READ_GEN_OLDEST;

err:	if (ret != 0) {
		__wt_free(session, new_ins_head_list);
		__wt_free(session, new_ins_head);
		if (new_parent != NULL)
			__wt_page_out(session, &new_parent);
		if (right_child != NULL)
			__wt_page_out(session, &right_child);
	}
	return (ret);
}

/*
 * __wt_split_page_inmem --
 *	Split a large in memory page into an in memory tree of pages. We must
 *	have exclusive access to the page upon entry.
 */
int
__wt_split_page_inmem(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_ASSERT(session, page->type == WT_PAGE_ROW_LEAF);

	WT_VERBOSE_RET(session, evict,
	    "Splitting page %p (%s)", page, __wt_page_type_string(page->type));

	return (__split_row_page_inmem(session, page));
}
