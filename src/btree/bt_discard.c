/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_discard_page_col_fix(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_int(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_rle(SESSION *, WT_PAGE *);
static void __wt_discard_page_col_var(SESSION *, WT_PAGE *);
static void __wt_discard_page_row_int(SESSION *, WT_PAGE *);
static void __wt_discard_page_row_leaf(SESSION *, WT_PAGE *);
static void __wt_discard_relexp(SESSION *, WT_PAGE *);
static void __wt_discard_update(SESSION *, WT_UPDATE **, uint32_t);
static void __wt_discard_update_list(SESSION *, WT_UPDATE *);
static inline int __wt_row_key_on_page(WT_PAGE *, void *);

/*
 * __wt_row_key_on_page --
 *	Return if a WT_ROW structure's key references on-page data.
 */
static inline int
__wt_row_key_on_page(WT_PAGE *page, void *key)
{
	uint8_t *p;

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	p = ((WT_ROW *)key)->key;
	return (p >= (uint8_t *)page->dsk &&
	    p < (uint8_t *)page->dsk + page->size ? 1 : 0);
}

/*
 * __wt_page_discard --
 *	Free all memory associated with a page.
 */
void
__wt_page_discard(SESSION *session, WT_PAGE *page)
{
	/* Never discard a dirty page. */
	WT_ASSERT(session, !WT_PAGE_IS_MODIFIED(page));

	switch (page->dsk->type) {
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

	if (page->dsk != NULL)
		__wt_free(session, page->dsk, page->size);
	__wt_free(session, page, sizeof(WT_PAGE));
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
		__wt_free(session,
		    page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

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
		__wt_free(session,
		    page->u.col_int.t, page->indx_count * sizeof(WT_COL_REF));
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
		__wt_free(session,
		    page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

	/* Free the run-length encoded column-store expansion array. */
	if (page->u.col_leaf.rleexp != NULL)
		__wt_discard_relexp(session, page);
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
		__wt_free(session,
		    page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

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
		if (!__wt_row_key_on_page(page, rref))
			__wt_free(session, rref->key, rref->size);

	/* Free the subtree-reference array. */
	if (page->u.row_int.t != NULL)
		__wt_free(session,
		    page->u.row_int.t, page->indx_count * sizeof(WT_ROW_REF));
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
	WT_ROW_INDX_FOREACH(page, rip, i)
		if (!__wt_row_key_on_page(page, rip))
			__wt_free(session, rip->key, rip->size);
	__wt_free(session,
	    page->u.row_leaf.d, page->indx_count * sizeof(WT_ROW));

	if (page->u.row_leaf.upd != NULL)
		__wt_discard_update(
		    session, page->u.row_leaf.upd, page->indx_count);
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
	__wt_free(session, update_head, indx_count * sizeof(WT_UPDATE *));
}

/*
 * __wt_discard_relexp --
 *	Discard the run-length encoded column-store expansion array.
 */
static void
__wt_discard_relexp(SESSION *session, WT_PAGE *page)
{
	WT_RLE_EXPAND **expp, *exp, *a;
	u_int i;

	/*
	 * For each non-NULL slot in the page's run-length encoded column
	 * store expansion array, free the linked list of WT_RLE_EXPAND
	 * structures anchored in that slot.
	 */
	WT_RLE_EXPAND_FOREACH(page, expp, i) {
		if ((exp = *expp) == NULL)
			continue;
		/*
		 * Free the linked list of WT_UPDATE structures anchored in the
		 * WT_RLE_EXPAND entry.
		 */
		__wt_discard_update_list(session, exp->upd);
		do {
			a = exp->next;
			__wt_free(session, exp, sizeof(WT_RLE_EXPAND));
		} while ((exp = a) != NULL);
	}

	/* Free the page's expansion array. */
	__wt_free(session, page->u.col_leaf.rleexp,
	    page->indx_count * sizeof(WT_RLE_EXPAND *));
}

/*
 * __wt_discard_update_list --
 *	Walk a WT_UPDATE forward-linked list and free the per-thread combination
 *	of a WT_UPDATE structure and its associated data.
 */
static void
__wt_discard_update_list(SESSION *session, WT_UPDATE *upd)
{
	WT_UPDATE *a;
	SESSION_BUFFER *sb;

	do {
		a = upd->next;

		sb = upd->sb;
		WT_ASSERT(session, sb->out < sb->in);
		if (++sb->out == sb->in)
			__wt_free(session, sb, sb->len);
	} while ((upd = a) != NULL);
}
