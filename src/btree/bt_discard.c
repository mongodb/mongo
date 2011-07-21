/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_free_insert(WT_SESSION_IMPL *, WT_INSERT **, uint32_t);
static void __wt_free_insert_list(WT_SESSION_IMPL *, WT_INSERT *);
static void __wt_free_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_free_page_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_free_page_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_free_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_free_update(WT_SESSION_IMPL *, WT_UPDATE **, uint32_t);
static void __wt_free_update_list(WT_SESSION_IMPL *, WT_UPDATE *);

/*
 * __wt_page_free --
 *	Free all memory associated with a page.
 */
void
__wt_page_free(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
#ifdef HAVE_DIAGNOSTIC
	__wt_hazard_validate(session, page);
#endif

	/*
	 * When a page is discarded, it's been disconnected from its parent
	 * (both page and WT_REF structure), and the parent's WT_REF structure
	 * may now reference a different page.   Make sure we don't use any of
	 * that information.
	 */
	page->parent = NULL;
	page->parent_ref = NULL;

	/*
	 * The page must either be clean, or an internal split page, which
	 * is created dirty and can never be "clean".
	 */
	WT_ASSERT(session,
	    F_ISSET(page, WT_PAGE_MERGE) || !WT_PAGE_IS_MODIFIED(page));

	/*
	 * If this page has a memory footprint associated with it, update
	 * the cache information.
	 */
	if (page->memory_footprint != 0)
		__wt_cache_page_evict(session, page);

	/* Bulk-loaded pages are skeleton pages, we don't need to do much. */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD))
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			__wt_free_update_list(session, page->u.bulk.upd);
			page->u.bulk.upd = NULL;
			break;
		case WT_PAGE_ROW_LEAF:
			__wt_free_insert_list(session, page->u.bulk.ins);
			page->u.bulk.ins = NULL;
			break;
		}
	else
		switch (page->type) {
		case WT_PAGE_COL_FIX:
			__wt_free_page_col_fix(session, page);
			break;
		case WT_PAGE_COL_INT:
			__wt_free_page_col_int(session, page);
			break;
		case WT_PAGE_COL_VAR:
			__wt_free_page_col_var(session, page);
			break;
		case WT_PAGE_ROW_INT:
			__wt_free_page_row_int(session, page);
			break;
		case WT_PAGE_ROW_LEAF:
			__wt_free_page_row_leaf(session, page);
			break;
		}

	if (!LF_ISSET(WT_PAGE_FREE_IGNORE_DISK) && page->dsk != NULL)
		__wt_free(session, page->dsk);
	__wt_free(session, page);
}

/*
 * __wt_free_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__wt_free_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the update array. */
	if (page->u.col_leaf.upd != NULL)
		__wt_free_update(session,
		    page->u.col_leaf.upd, page->entries);
}

/*
 * __wt_free_page_col_int --
 *	Discard a WT_PAGE_COL_INT page.
 */
static void
__wt_free_page_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Free the subtree-reference array. */
	if (page->u.col_int.t != NULL)
		__wt_free(session, page->u.col_int.t);
}

/*
 * __wt_free_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__wt_free_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the insert array. */
	if (page->u.col_leaf.ins != NULL)
		__wt_free_insert(session, page->u.col_leaf.ins, page->entries);
}

/*
 * __wt_free_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__wt_free_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * For each referenced key, see if the key was an allocation (that is,
	 * if it points somewhere other than the original page), and free it.
	 */
	WT_ROW_REF_FOREACH(page, rref, i)
		if ((ikey = rref->key) != NULL)
			__wt_sb_free(session, ikey->sb);

	/* Free the subtree-reference array. */
	if (page->u.row_int.t != NULL)
		__wt_free(session, page->u.row_int.t);
}

/*
 * __wt_free_page_row_leaf --
 *	Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__wt_free_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
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
		if ((ikey = rip->key) != NULL && __wt_off_page(page, ikey))
			__wt_sb_free(session, ikey->sb);
	__wt_free(session, page->u.row_leaf.d);

	/*
	 * Free the insert array.
	 *
	 * Row-store tables have one additional slot in the insert array (the
	 * insert array has an extra slot to hold keys that sort before keys
	 * found on the original page).
	 */
	if (page->u.row_leaf.ins != NULL)
		__wt_free_insert(
		    session, page->u.row_leaf.ins, page->entries + 1);

	/* Free the update array. */
	if (page->u.row_leaf.upd != NULL)
		__wt_free_update(
		    session, page->u.row_leaf.upd, page->entries);
}

/*
 * __wt_free_insert --
 *	Discard the insert array.
 */
static void
__wt_free_insert(
    WT_SESSION_IMPL *session, WT_INSERT **insert_head, uint32_t entries)
{
	WT_INSERT **insp;

	/*
	 * For each non-NULL slot in the page's array of inserts, free the
	 * linked list anchored in that slot.
	 */
	for (insp = insert_head; entries > 0; --entries, ++insp)
		if (*insp != NULL)
			__wt_free_insert_list(session, *insp);

	/* Free the page's array of inserts. */
	__wt_free(session, insert_head);
}

/*
 * __wt_free_insert_list --
 *	Walk a WT_INSERT forward-linked list and free the per-thread combination
 * of a WT_INSERT structure and its associated chain of WT_UPDATE structures.
 */
static void
__wt_free_insert_list(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	WT_INSERT *next;

	do {
		__wt_free_update_list(session, ins->upd);

		next = ins->next;
		__wt_sb_free(session, ins->sb);
	} while ((ins = next) != NULL);
}

/*
 * __wt_free_update --
 *	Discard the update array.
 */
static void
__wt_free_update(
    WT_SESSION_IMPL *session, WT_UPDATE **update_head, uint32_t entries)
{
	WT_UPDATE **updp;

	/*
	 * For each non-NULL slot in the page's array of updates, free the
	 * linked list anchored in that slot.
	 */
	for (updp = update_head; entries > 0; --entries, ++updp)
		if (*updp != NULL)
			__wt_free_update_list(session, *updp);

	/* Free the page's array of updates. */
	__wt_free(session, update_head);
}

/*
 * __wt_free_update_list --
 *	Walk a WT_UPDATE forward-linked list and free the per-thread combination
 *	of a WT_UPDATE structure and its associated data.
 */
static void
__wt_free_update_list(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	do {
		next = upd->next;
		__wt_sb_free(session, upd->sb);
	} while ((upd = next) != NULL);
}
