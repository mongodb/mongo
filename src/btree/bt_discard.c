/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_page_discard_rleexp(ENV *, WT_PAGE *);
static void __wt_page_discard_repl(ENV *, WT_PAGE *);
static void __wt_page_discard_repl_list(ENV *, WT_REPL *);
static inline int __wt_row_key_on_page(WT_PAGE *, WT_ROW *);

/*
 * __wt_page_discard --
 *	Free all memory associated with a page.
 */
void
__wt_page_discard(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_ROW *rip;
	uint32_t i, type;
	void *last_key;

	env = toc->env;
	type = page->dsk->type;

	/* Never discard a dirty page. */
	WT_ASSERT(env, !WT_PAGE_IS_MODIFIED(page));

	/* Free the in-memory index array. */
	switch (type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		__wt_free(env, page->u.icol, page->indx_count * sizeof(WT_COL));
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		/*
		 * For each entry, see if the key was an allocation (that is,
		 * if it points somewhere other than the original page), and
		 * if so, free the memory.
		 */
		last_key = NULL;
		WT_INDX_FOREACH(page, rip, i)
			if (!__wt_row_key_on_page(page, rip))
				__wt_free(env, rip->key, rip->size);
		__wt_free(env, page->u.irow, page->indx_count * sizeof(WT_ROW));
		break;
	}

	/* Free the modified/deletion replacements array. */
	switch (type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		if (page->u2.repl != NULL)
			__wt_page_discard_repl(env, page);
		break;
	}

	/* Free the run-length encoded column-store expansion array. */
	switch (type) {
	case WT_PAGE_COL_RLE:
		if (page->u2.rleexp != NULL)
			__wt_page_discard_rleexp(env, page);
		break;
	}

	/* Free the subtree-reference array. */
	switch (type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		if (page->u2.ref != NULL)
			__wt_free(env, page->u2.ref,
			    page->indx_count * sizeof(WT_REF));
		break;
	}

	if (page->dsk != NULL)
		__wt_free(env, page->dsk, page->size);
	__wt_free(env, page, sizeof(WT_PAGE));
}

/*
 * __wt_page_discard_repl --
 *	Discard the replacement array.
 */
static void
__wt_page_discard_repl(ENV *env, WT_PAGE *page)
{
	WT_REPL **replp;
	u_int i;

	/*
	 * For each non-NULL slot in the page's array of replacements, free the
	 * linked list anchored in that slot.
	 */
	WT_REPL_FOREACH(page, replp, i)
		if (*replp != NULL)
			__wt_page_discard_repl_list(env, *replp);

	/* Free the page's array of replacements. */
	__wt_free(env, page->u2.repl, page->indx_count * sizeof(WT_REPL *));
}

/*
 * __wt_page_discard_rleexp --
 *	Discard the run-length encoded column-store expansion array.
 */
static void
__wt_page_discard_rleexp(ENV *env, WT_PAGE *page)
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
		__wt_page_discard_repl_list(env, exp->repl);
		do {
			a = exp->next;
			__wt_free(env, exp, sizeof(WT_RLE_EXPAND));
		} while ((exp = a) != NULL);
	}

	/* Free the page's expansion array. */
	__wt_free(
	    env, page->u2.rleexp, page->indx_count * sizeof(WT_RLE_EXPAND *));
}

/*
 * __wt_page_discard_repl_list --
 *	Walk a WT_REPL forward-linked list and free the per-thread combination
 *	of a WT_REPL structure and its associated data.
 */
static void
__wt_page_discard_repl_list(ENV *env, WT_REPL *repl)
{
	WT_REPL *a;
	WT_TOC_UPDATE *update;

	do {
		a = repl->next;

		update = repl->update;
		WT_ASSERT(env, update->out < update->in);
		if (++update->out == update->in)
			__wt_free(env, update, update->len);
	} while ((repl = a) != NULL);
}

/*
 * __wt_row_key_on_page --
 *	Return if a WT_ROW structure's key references on-page data.
 */
static inline int
__wt_row_key_on_page(WT_PAGE *page, WT_ROW *rip)
{
	uint8_t *p;

	p = rip->key;
	return (p >= (uint8_t *)page->dsk &&
	    p < (uint8_t *)page->dsk + page->size ? 1 : 0);
}
