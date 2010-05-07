/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_page_discard_repl(ENV *, WT_REPL *);
static int  __wt_bt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_col_var(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_row(WT_TOC *, WT_PAGE *, WT_PAGE *);

/*
 * __wt_bt_rec_page --
 *	Move a page from its in-memory state out to disk.
 */
int
__wt_bt_rec_page(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE *rp;
	WT_PAGE_HDR *hdr;
	u_int32_t max;

	db = toc->db;
	env = toc->env;
	cache = env->ienv->cache;
	hdr = page->hdr;

	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_OVFL:
		return (__wt_page_write(db, page));
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		/* We'll potentially need a new leaf page. */
		max = db->leafmax;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		/* We'll potentially need a new internal page. */
		max = db->intlmax;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * There's one reconciliation buffer, stored in the cache structure.
	 * Make sure it's big enough.
	 */
	if (cache->recbuf_size < max)
		WT_RET(__wt_realloc(
		    env, &cache->recbuf_size, max, &cache->recbuf));

	/* Initialize the reconciliation buffer as a replacement page. */
	rp = (WT_PAGE *)cache->recbuf;
	rp->size = max;
	memcpy(cache->recbuf, page->hdr, WT_PAGE_HDR_SIZE);
	__wt_bt_set_ff_and_sa_from_addr(rp, WT_PAGE_BYTE(rp));

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_bt_rec_col_fix(toc, page, rp));
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_bt_rec_col_var(toc, page, rp));
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_bt_rec_int(toc, page, rp));
		break;
	case WT_PAGE_ROW_LEAF:
	case WT_PAGE_DUP_LEAF:
		WT_RET(__wt_bt_rec_row(toc, page, rp));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	return (0);
}

/*
 * __wt_bt_rec_col_fix --
 *	Reconcile a fixed-width column-store leaf page.
 */
static int
__wt_bt_rec_col_fix(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
	WT_COL_INDX *cip;
	u_int32_t i;

rp = NULL;
	WT_INDX_FOREACH(page, cip, i)
		;
	return (__wt_page_write(toc->db, page));
}

/*
 * __wt_bt_rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__wt_bt_rec_col_var(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
	WT_COL_INDX *cip;
	u_int32_t i;

rp = NULL;
	WT_INDX_FOREACH(page, cip, i)
		;
	return (__wt_page_write(toc->db, page));
}

/*
 * __wt_bt_rec_int --
 *	Reconcile an internal page.
 */
static int
__wt_bt_rec_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
rp = NULL;
	return (__wt_page_write(toc->db, page));
}

/*
 * __wt_bt_rec_row --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_bt_rec_row(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
rp = NULL;
#if 0
	ENV *env;
	WT_ROW_INDX *rip;
	u_int32_t i, len;
	u_int8_t *acc;

	env = db->env;

	acc = NULL;
	WT_INDX_FOREACH(page, rip, i) {
		/*
		 * If the item is unchanged, we can take it off the page.  We
		 * accumulate items for as long as possible, then copy them in
		 * one shot.
		 */
		if (rip->repl == NULL) {
			if (acc == NULL)
				acc = rip->page_data;
			continue;
		}
		if (acc != NULL) {
			len = (u_int8_t *)rip->page_data - acc;
			memcpy(rp, acc, len);
			rp += len;
		}
	}

	return (__wt_page_write(db, rp));
#else
	return (__wt_page_write(toc->db, page));
#endif
}

/*
 * __wt_bt_page_discard --
 *	Free all memory associated with a page.
 */
void
__wt_bt_page_discard(ENV *env, WT_PAGE *page)
{
	WT_COL_INDX *cip;
	WT_ROW_INDX *rip;
	u_int32_t i;
	void *bp, *ep;

	WT_ENV_FCHK_ASSERT(
	    env, "__wt_bt_page_discard", page->flags, WT_APIMASK_WT_PAGE);

	switch (page->hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_INDX_FOREACH(page, cip, i)
			if (cip->repl != NULL)
				__wt_bt_page_discard_repl(env, cip->repl);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		bp = (u_int8_t *)page->hdr;
		ep = (u_int8_t *)bp + page->size;
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * For each entry, see if the data was an allocation,
			 * that is, if it points somewhere other than the
			 * original page.  If it's an allocation, free it.
			 *
			 * For each entry, see if replacements were made -- if
			 * so, free them.
			 */
			if (rip->data < bp || rip->data >= ep)
				__wt_free(env, rip->data, rip->size);
			if (rip->repl != NULL)
				__wt_bt_page_discard_repl(env, rip->repl);
		}
		break;
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_OVFL:
	default:
		break;
	}

	if (page->u.indx != NULL)
		__wt_free(env, page->u.indx, 0);

	__wt_free(env, page->hdr, page->size);
	__wt_free(env, page, sizeof(WT_PAGE));
}

/*
 * __wt_bt_page_discard_repl --
 *	Discard the replacement array.
 */
static void
__wt_bt_page_discard_repl(ENV *env, WT_REPL *repl)
{
	u_int16_t i;

	/* Free the data pointers and then the WT_REPL structure itself. */
	for (i = 0; i < repl->repl_next; ++i)
		if (repl->data[i].data != WT_DATA_DELETED)
			__wt_free(env, repl->data[i].data, repl->data[i].size);
	__wt_free(env, repl->data, repl->repl_size * sizeof(WT_SDBT));
	__wt_free(env, repl, sizeof(WT_REPL));
}
