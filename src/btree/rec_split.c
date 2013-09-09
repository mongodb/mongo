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
	WT_INSERT *current_ins, *first_ins, *ins, **insp, *prev_ins;
	WT_INSERT_HEAD **new_ins_head_list;
	WT_INSERT_HEAD *ins_head, *new_ins_head, *orig_ins_head;
	WT_ITEM key;
	WT_PAGE *new_parent, *right_child;
	WT_REF *newref;
	WT_UPDATE *next_upd;
	int i, ins_depth;
	size_t transfer_size;

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
#define	WT_MIN_SPLIT_SKIPLIST_DEPTH	WT_MIN(5, WT_SKIP_MAXDEPTH - 1)
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
	right_child->u.row.ins = new_ins_head_list;
	WT_ERR(__wt_calloc_def(session, 1, &new_ins_head));
	right_child->u.row.ins[0] = new_ins_head;
	__wt_cache_page_inmem_incr(session,
	    right_child, sizeof(WT_INSERT_HEAD) + sizeof(WT_INSERT_HEAD *));

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
	    WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), &newref->key.ikey));

	/*
	 * Copy the first key from the original page into first ref in the new
	 * parent.  Pages created in memory always have a "smallest" insert
	 * list, so look there first.  If we don't find one, get the first key
	 * from the disk image.
	 */
	if ((orig_ins_head = WT_ROW_INSERT_SMALLEST(orig)) != NULL &&
	    (first_ins = WT_SKIP_FIRST(orig_ins_head)) != NULL) {
		key.data = WT_INSERT_KEY(first_ins);
		key.size = WT_INSERT_KEY_SIZE(first_ins);
	} else {
		WT_ASSERT(session, orig->dsk != NULL && orig->entries != 0);
		WT_ERR(
		    __wt_row_leaf_key(session, orig, orig->u.row.d, &key, 1));
	}
	newref = &new_parent->u.intl.t[0];
	WT_ERR(__wt_row_ikey_incr(session, new_parent, 0,
	    key.data, key.size, &newref->key.ikey));

	/*
	 * !!! All operations that can fail have now completely successfully.
	 * Link the original page into the new parent.
	 */
	WT_LINK_PAGE(new_parent, newref, orig);
	newref->state = WT_REF_MEM;

	/*
	 * Figure out how deep the skip list stack is for the element we are
	 * removing.
	 */
	for (ins_depth = 0;
	    ins_depth < WT_SKIP_MAXDEPTH && ins_head->tail[ins_depth] == ins;
	    ins_depth++)
		;

	/*
	 * Now that all operations that could fail have completed, we start
	 * updating the original page.
	 */
	transfer_size =
	    ((size_t)(ins_depth - 1) * sizeof(WT_INSERT *)) +
	    sizeof(WT_INSERT) + WT_INSERT_KEY_SIZE(ins) + ins->upd->size;
	for (next_upd = ins->upd->next;
	    next_upd != NULL; next_upd = next_upd->next)
		transfer_size += next_upd->size;
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

	/* Install the item on the new child page. */
	for (i = 0; i < ins_depth; i++)
		new_ins_head->head[i] = new_ins_head->tail[i] = ins;

	/* Make it likely we evict the page we just split. */
	orig->read_gen = WT_READ_GEN_OLDEST;

	/*
	 * Swap the new top-level page into place.  This must come last: once
	 * the parent is unlocked, it isn't safe to touch any of these pages.
	 */
	WT_LINK_PAGE(new_parent->parent, new_parent->ref, new_parent);
	WT_PUBLISH(new_parent->ref->state, WT_REF_MEM);

err:	if (ret != 0) {
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
