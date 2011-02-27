/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_discard_page_col_fix(ENV *, WT_PAGE *);
static void __wt_discard_page_col_int(ENV *, WT_PAGE *);
static void __wt_discard_page_col_rle(ENV *, WT_PAGE *);
static void __wt_discard_page_col_var(ENV *, WT_PAGE *);
static void __wt_discard_page_row_int(ENV *, WT_PAGE *);
static void __wt_discard_page_row_leaf(ENV *, WT_PAGE *);
static void __wt_discard_relexp(ENV *, WT_PAGE *);
static void __wt_discard_repl(ENV *, WT_REPL **, uint32_t);
static void __wt_discard_repl_list(ENV *, WT_REPL *);
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
int
__wt_page_discard(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;

	db = toc->db;
	env = toc->env;

	/* Never discard a dirty page. */
	WT_ASSERT(env, !WT_PAGE_IS_MODIFIED(page));

	switch (page->dsk->type) {
	case WT_PAGE_COL_FIX:
		__wt_discard_page_col_fix(env, page);
		break;
	case WT_PAGE_COL_INT:
		__wt_discard_page_col_int(env, page);
		break;
	case WT_PAGE_COL_RLE:
		__wt_discard_page_col_rle(env, page);
		break;
	case WT_PAGE_COL_VAR:
		__wt_discard_page_col_var(env, page);
		break;
	case WT_PAGE_ROW_INT:
		__wt_discard_page_row_int(env, page);
		break;
	case WT_PAGE_ROW_LEAF:
		__wt_discard_page_row_leaf(env, page);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (page->dsk != NULL)
		__wt_free(env, page->dsk, page->size);
	__wt_free(env, page, sizeof(WT_PAGE));

	return (0);
}

/*
 * __wt_discard_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__wt_discard_page_col_fix(ENV *env, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(
		    env, page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

	/* Free the modified/deletion replacements array. */
	if (page->u.col_leaf.repl != NULL)
		__wt_discard_repl(env, page->u.col_leaf.repl, page->indx_count);
}

/*
 * __wt_discard_page_col_int --
 *	Discard a WT_PAGE_COL_INT page.
 */
static void
__wt_discard_page_col_int(ENV *env, WT_PAGE *page)
{
	/* Free the subtree-reference array. */
	if (page->u.col_int.t != NULL)
		__wt_free(env,
		    page->u.col_int.t, page->indx_count * sizeof(WT_COL_REF));
}

/*
 * __wt_discard_page_col_rle --
 *	Discard a WT_PAGE_COL_RLE page.
 */
static void
__wt_discard_page_col_rle(ENV *env, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(
		    env, page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

	/* Free the run-length encoded column-store expansion array. */
	if (page->u.col_leaf.rleexp != NULL)
		__wt_discard_relexp(env, page);
}

/*
 * __wt_discard_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__wt_discard_page_col_var(ENV *env, WT_PAGE *page)
{
	/* Free the in-memory index array. */
	if (page->u.col_leaf.d != NULL)
		__wt_free(
		    env, page->u.col_leaf.d, page->indx_count * sizeof(WT_COL));

	/* Free the modified/deletion replacements array. */
	if (page->u.col_leaf.repl != NULL)
		__wt_discard_repl(env, page->u.col_leaf.repl, page->indx_count);
}

/*
 * __wt_discard_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__wt_discard_page_row_int(ENV *env, WT_PAGE *page)
{
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * For each referenced key, see if the key was an allocation (that is,
	 * if it points somewhere other than the original page), and free it.
	 */
	WT_ROW_REF_FOREACH(page, rref, i)
		if (!__wt_row_key_on_page(page, rref))
			__wt_free(env, rref->key, rref->size);

	/* Free the subtree-reference array. */
	if (page->u.row_int.t != NULL)
		__wt_free(env,
		    page->u.row_int.t, page->indx_count * sizeof(WT_ROW_REF));
}

/*
 * __wt_discard_page_row_leaf --
 *	Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__wt_discard_page_row_leaf(ENV *env, WT_PAGE *page)
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
			__wt_free(env, rip->key, rip->size);
	__wt_free(env, page->u.row_leaf.d, page->indx_count * sizeof(WT_ROW));

	if (page->u.row_leaf.repl != NULL)
		__wt_discard_repl(env, page->u.row_leaf.repl, page->indx_count);
}

/*
 * __wt_discard_repl --
 *	Discard the replacement array.
 */
static void
__wt_discard_repl(ENV *env, WT_REPL **replp_head, uint32_t indx_count)
{
	WT_REPL **replp;

	/*
	 * For each non-NULL slot in the page's array of replacements, free the
	 * linked list anchored in that slot.
	 */
	for (replp = replp_head; indx_count > 0; --indx_count, ++replp)
		if (*replp != NULL)
			__wt_discard_repl_list(env, *replp);

	/* Free the page's array of replacements. */
	__wt_free(env, replp_head, indx_count * sizeof(WT_REPL *));
}

/*
 * __wt_discard_relexp --
 *	Discard the run-length encoded column-store expansion array.
 */
static void
__wt_discard_relexp(ENV *env, WT_PAGE *page)
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
		 * Free the linked list of WT_REPL structures anchored in the
		 * WT_RLE_EXPAND entry.
		 */
		__wt_discard_repl_list(env, exp->repl);
		do {
			a = exp->next;
			__wt_free(env, exp, sizeof(WT_RLE_EXPAND));
		} while ((exp = a) != NULL);
	}

	/* Free the page's expansion array. */
	__wt_free(env, page->u.col_leaf.rleexp,
	    page->indx_count * sizeof(WT_RLE_EXPAND *));
}

/*
 * __wt_discard_repl_list --
 *	Walk a WT_REPL forward-linked list and free the per-thread combination
 *	of a WT_REPL structure and its associated data.
 */
static void
__wt_discard_repl_list(ENV *env, WT_REPL *repl)
{
	WT_REPL *a;
	WT_TOC_BUFFER *tb;

	do {
		a = repl->next;

		tb = repl->tb;
		WT_ASSERT(env, tb->out < tb->in);
		if (++tb->out == tb->in)
			__wt_free(env, tb, tb->len);
	} while ((repl = a) != NULL);
}
