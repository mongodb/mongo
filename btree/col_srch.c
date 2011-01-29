/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_search_col --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_bt_search_col(WT_TOC *toc, uint64_t recno, uint32_t level, uint32_t flags)
{
	DB *db;
	IDB *idb;
	WT_COL *cip;
	WT_OFF *off;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_RCC_EXPAND *exp;
	WT_REF *ref;
	WT_REPL *repl;
	uint64_t record_cnt;
	uint32_t i, write_gen;
	int ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;
	toc->srch_repl = NULL;
	toc->srch_exp = NULL;
	toc->srch_write_gen = 0;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db, "__wt_bt_search_col", flags, WT_APIMASK_BT_SEARCH_COL);

	/* Check for a record past the end of the database. */
	page = idb->root_page.page;
	if (page->records < recno)
		return (WT_NOTFOUND);

	/* Search the tree. */
	for (;;) {
		/* Save the write generation value before the read. */
		write_gen = page->write_gen;

		/* Walk the page looking for the record. */
		hdr = page->hdr;
		switch (hdr->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			cip = page->u.icol + (recno - hdr->start_recno);
			goto done;
		case WT_PAGE_COL_RCC:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - hdr->start_recno;
			WT_INDX_FOREACH(page, cip, i) {
				if (record_cnt < WT_RCC_REPEAT_COUNT(cip->data))
					break;
				record_cnt -= WT_RCC_REPEAT_COUNT(cip->data);
			}
			goto done;
		case WT_PAGE_COL_INT:
		default:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - hdr->start_recno;
			WT_INDX_FOREACH(page, cip, i) {
				if (record_cnt < WT_COL_OFF_RECORDS(cip))
					break;
				record_cnt -= WT_COL_OFF_RECORDS(cip);
			}
			break;
		}

		/* If a level was set, see if we found the asked-for page. */
		if (level == hdr->level)
			goto done;

		/* cip references the subtree containing the record. */
		ref = WT_COL_REF(page, cip);
		off = WT_COL_OFF(cip);
		WT_ERR(__wt_bt_page_in(toc, page, ref, off, 0));

		/* Swap the parent page for the child page. */
		if (page != idb->root_page.page)
			__wt_hazard_clear(toc, page);
		page = ref->page;
	}

done:	/*
	 * We've found the right on-page WT_COL structure, but that's only the
	 * first step; the record may have been updated since reading the page
	 * into the cache.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		/* Find the item's WT_REPL slot if it exists. */
		repl = WT_COL_REPL(page, cip);

		/*
		 * If overwriting an existing data item, we don't care if the
		 * item was previously deleted, return the gathered information.
		 */
		if (LF_ISSET(WT_DATA_OVERWRITE)) {
			toc->srch_repl = repl;
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_REPL slot
		 * or in the original data.
		 */
		if (repl != NULL) {
			if (WT_REPL_DELETED_ISSET(repl))
				goto notfound;
			toc->srch_repl = repl;
		} else
			if (WT_FIX_DELETE_ISSET(cip->data))
				goto notfound;
		break;
	case WT_PAGE_COL_RCC:
		/* Find the item's WT_COL_EXP slot if it exists. */
		for (exp =
		    WT_COL_RCCEXP(page, cip); exp != NULL; exp = exp->next)
			if (exp->recno == recno)
				break;

		/*
		 * If overwriting an existing data item, we don't care if the
		 * item was previously deleted, return the gathered information.
		 */
		if (LF_ISSET(WT_DATA_OVERWRITE)) {
			if (exp != NULL) {
				toc->srch_exp = exp;
				toc->srch_repl = exp->repl;
			}
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_REPL slot
		 * (referenced by the WT_COL_EXP slot), or in the original data.
		 */
		if (exp != NULL) {
			if (WT_REPL_DELETED_ISSET(exp->repl))
				goto notfound;
			toc->srch_exp = exp;
			toc->srch_repl = exp->repl;
		} else
			if (WT_FIX_DELETE_ISSET(WT_RCC_REPEAT_DATA(cip->data)))
				goto notfound;
		break;
	case WT_PAGE_COL_VAR:
		/* Find the item's WT_REPL slot if it exists. */
		repl = WT_COL_REPL(page, cip);

		/*
		 * If overwriting an existing data item, we don't care if the
		 * item was previously deleted, return the gathered information.
		 */
		if (LF_ISSET(WT_DATA_OVERWRITE)) {
			toc->srch_repl = repl;
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_REPL slot
		 * or in the original data.
		 */
		if (repl != NULL) {
			if (WT_REPL_DELETED_ISSET(repl))
				goto notfound;
			toc->srch_repl = repl;
			break;
		} else
			if (WT_ITEM_TYPE(cip->data) == WT_ITEM_DEL)
				goto notfound;
		break;
	case WT_PAGE_COL_INT:
		/*
		 * When returning internal pages, set the item's WT_REPL slot
		 * if it exists, otherwise we're done.
		 */
		toc->srch_repl = WT_COL_REPL(page, cip);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	toc->srch_page = page;
	toc->srch_ip = cip;
	toc->srch_write_gen = write_gen;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	WT_PAGE_OUT(toc, page);
	return (ret);
}
