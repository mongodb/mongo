/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static void __wt_free_insert(SESSION *, WT_INSERT **, uint32_t);
static void __wt_free_insert_list(SESSION *, WT_INSERT *);
static void __wt_free_page_col_fix(SESSION *, WT_PAGE *);
static void __wt_free_page_col_int(SESSION *, WT_PAGE *);
static void __wt_free_page_col_rle(SESSION *, WT_PAGE *);
static void __wt_free_page_col_var(SESSION *, WT_PAGE *);
static void __wt_free_page_row_int(SESSION *, WT_PAGE *, uint32_t);
static void __wt_free_page_row_leaf(SESSION *, WT_PAGE *, uint32_t);
static void __wt_free_update(SESSION *, WT_UPDATE **, uint32_t);
static void __wt_free_update_list(SESSION *, WT_UPDATE *);

/*
 * __wt_page_free --
 *	Free all memory associated with a page.
 */
void
__wt_page_free(
    SESSION *session, WT_PAGE *page, uint32_t addr, uint32_t size)
{
	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "discard addr %lu/%lu (type %s)",
	    (u_long)addr, (u_long)size, __wt_page_type_string(page->type)));

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
	 * The address/size may not be set, and that's OK, but it means there
	 * better not be any disk image associated with this page.
	 */
	WT_ASSERT(session,
	    (addr != WT_ADDR_INVALID && size != 0) || page->dsk == NULL);

	/*
	 * The page must either be clean, or an internal split page, which
	 * is created dirty and can never be "clean".
	 */
	WT_ASSERT(session,
	    F_ISSET(page, WT_PAGE_SPLIT) || !WT_PAGE_IS_MODIFIED(page));

	/*
	 * Pages created in memory aren't counted against our cache limit; if
	 * this was originally a disk-backed page read by the read server, we
	 * have more space.
	 */
	if (F_ISSET(page, WT_PAGE_CACHE_COUNTED))
		__wt_cache_page_out(session, page, size);

	/* Bulk-loaded pages are skeleton pages, we don't need to do much. */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD))
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_RLE:
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
		case WT_PAGE_COL_RLE:
			__wt_free_page_col_rle(session, page);
			break;
		case WT_PAGE_COL_VAR:
			__wt_free_page_col_var(session, page);
			break;
		case WT_PAGE_ROW_INT:
			__wt_free_page_row_int(session, page, size);
			break;
		case WT_PAGE_ROW_LEAF:
			__wt_free_page_row_leaf(session, page, size);
			break;
		}

	if (page->dsk != NULL)
		__wt_free(session, page->dsk);
	__wt_free(session, page);
}

/*
 * __wt_free_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__wt_free_page_col_fix(SESSION *session, WT_PAGE *page)
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
__wt_free_page_col_int(SESSION *session, WT_PAGE *page)
{
	/* Free the subtree-reference array. */
	if (page->u.col_int.t != NULL)
		__wt_free(session, page->u.col_int.t);
}

/*
 * __wt_free_page_col_rle --
 *	Discard a WT_PAGE_COL_RLE page.
 */
static void
__wt_free_page_col_rle(SESSION *session, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(session, page->u.col_leaf.d);

	/* Free the insert array. */
	if (page->u.col_leaf.ins != NULL)
		__wt_free_insert(
		    session, page->u.col_leaf.ins, page->entries);
}

/*
 * __wt_free_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__wt_free_page_col_var(SESSION *session, WT_PAGE *page)
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
 * __wt_free_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__wt_free_page_row_int(SESSION *session, WT_PAGE *page, uint32_t size)
{
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * For each referenced key, see if the key was an allocation (that is,
	 * if it points somewhere other than the original page), and free it.
	 */
	WT_ROW_REF_FOREACH(page, rref, i)
		if (__wt_ref_off_page(page, rref->key, size))
			__wt_free(session, rref->key);

	/* Free the subtree-reference array. */
	if (page->u.row_int.t != NULL)
		__wt_free(session, page->u.row_int.t);
}

/*
 * __wt_free_page_row_leaf --
 *	Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__wt_free_page_row_leaf(SESSION *session, WT_PAGE *page, uint32_t size)
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
		if (__wt_ref_off_page(page, rip->key, size))
			__wt_free(session, rip->key);
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
    SESSION *session, WT_INSERT **insert_head, uint32_t entries)
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
__wt_free_insert_list(SESSION *session, WT_INSERT *ins)
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
    SESSION *session, WT_UPDATE **update_head, uint32_t entries)
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
__wt_free_update_list(SESSION *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	do {
		next = upd->next;
		__wt_sb_free(session, upd->sb);
	} while ((upd = next) != NULL);
}
