/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static void __wt_discard_insert(SESSION *, WT_INSERT **, uint32_t);
static void __wt_discard_page_col_fix(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_int(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_rle(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_var(SESSION *, WT_PAGE *);
static void __wt_discard_page_row_int(SESSION *, WT_PAGE *);
static void __wt_discard_page_row_leaf(SESSION *, WT_PAGE *);
static void __wt_discard_update(SESSION *, WT_UPDATE **, uint32_t);
static void __wt_discard_update_list(SESSION *, WT_UPDATE *);

/*
 * __wt_page_discard --
 *	Free all memory associated with a page.
 */
void
__wt_page_discard(SESSION *session, WT_PAGE *page)
{
	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "discard addr %lu (type %s)",
	    (u_long)page->addr, __wt_page_type_string(page->type)));

	/* We've got more space. */
	WT_CACHE_PAGE_OUT(S2C(session)->cache, page->size);

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		__wt_discard_page_col_fix(session, page);
		break;
	case WT_PAGE_COL_INT:
		__wt_discard_page_col_int(session, page);
		break;
	case WT_PAGE_COL_RLE:
		__wt_discard_page_col_rle(session, page);
		break;
	case WT_PAGE_COL_VAR:
		__wt_discard_page_col_var(session, page);
		break;
	case WT_PAGE_ROW_INT:
		__wt_discard_page_row_int(session, page);
		break;
	case WT_PAGE_ROW_LEAF:
		__wt_discard_page_row_leaf(session, page);
		break;
	}

	if (page->XXdsk != NULL)
		__wt_free(session, page->XXdsk);
	__wt_free(session, page);

}

/*
 * __wt_discard_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__wt_discard_page_col_fix(SESSION *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the update array. */
	if (page->u.col_leaf.upd != NULL)
		__wt_discard_update(session,
		    page->u.col_leaf.upd, page->indx_count);
}

/*
 * __wt_discard_page_col_int --
 *	Discard a WT_PAGE_COL_INT page.
 */
static void
__wt_discard_page_col_int(SESSION *session, WT_PAGE *page)
{
	/* Free the subtree-reference array. */
	if (page->u.col_int.t != NULL)
		__wt_free(session, page->u.col_int.t);
}

/*
 * __wt_discard_page_col_rle --
 *	Discard a WT_PAGE_COL_RLE page.
 */
static void
__wt_discard_page_col_rle(SESSION *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the insert array. */
	if (page->u.col_leaf.ins != NULL)
		__wt_discard_insert(
		    session, page->u.col_leaf.ins, page->indx_count);
}

/*
 * __wt_discard_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__wt_discard_page_col_var(SESSION *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the update array. */
	if (page->u.col_leaf.upd != NULL)
		__wt_discard_update(session,
		    page->u.col_leaf.upd, page->indx_count);
}

/*
 * __wt_discard_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__wt_discard_page_row_int(SESSION *session, WT_PAGE *page)
{
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * For each referenced key, see if the key was an allocation (that is,
	 * if it points somewhere other than the original page), and free it.
	 */
	WT_ROW_REF_FOREACH(page, rref, i)
		if (__wt_ref_off_page(page, rref->key))
			__wt_free(session, rref->key);

	/* Free the subtree-reference array. */
	if (page->u.row_int.t != NULL)
		__wt_free(session, page->u.row_int.t);
}

/*
 * __wt_discard_page_row_leaf --
 *	Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__wt_discard_page_row_leaf(SESSION *session, WT_PAGE *page)
{
	WT_ROW *rip;
	uint32_t i;

	/*
	 * Free the in-memory index array.
	 *
	 * For each entry, see if the key was an allocation (that is, if it
	 * points somewhere other than the original page), and if so, free
	 * the memory.
	 */
	WT_ROW_FOREACH(page, rip, i)
		if (__wt_ref_off_page(page, rip->key))
			__wt_free(session, rip->key);
	__wt_free(session, page->u.row_leaf.d);

	/* Free the insert array. */
	if (page->u.row_leaf.ins != NULL)
		__wt_discard_insert(
		    session, page->u.row_leaf.ins, page->indx_count);

	/* Free the update array. */
	if (page->u.row_leaf.upd != NULL)
		__wt_discard_update(
		    session, page->u.row_leaf.upd, page->indx_count);
}

/*
 * __wt_discard_insert --
 *	Discard the insert array.
 */
static void
__wt_discard_insert(
    SESSION *session, WT_INSERT **insert_head, uint32_t indx_count)
{
	WT_INSERT **insp, *ins, *next;

	/*
	 * For each non-NULL slot in the page's array of inserts, free the
	 * linked list anchored in that slot.
	 */
	for (insp = insert_head; indx_count > 0; --indx_count, ++insp)
		for (ins = *insp; ins != NULL; ins = next) {
			__wt_discard_update_list(session, ins->upd);

			next = ins->next;
			__wt_sb_free(session, ins->sb);
		}

	/* Free the page's array of inserts. */
	__wt_free(session, insert_head);
}

/*
 * __wt_discard_update --
 *	Discard the update array.
 */
static void
__wt_discard_update(
    SESSION *session, WT_UPDATE **update_head, uint32_t indx_count)
{
	WT_UPDATE **updp;

	/*
	 * For each non-NULL slot in the page's array of updates, free the
	 * linked list anchored in that slot.
	 */
	for (updp = update_head; indx_count > 0; --indx_count, ++updp)
		if (*updp != NULL)
			__wt_discard_update_list(session, *updp);

	/* Free the page's array of updates. */
	__wt_free(session, update_head);
}

/*
 * __wt_discard_update_list --
 *	Walk a WT_UPDATE forward-linked list and free the per-thread combination
 *	of a WT_UPDATE structure and its associated data.
 */
static void
__wt_discard_update_list(SESSION *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	do {
		next = upd->next;
		__wt_sb_free(session, upd->sb);
	} while ((upd = next) != NULL);
}
