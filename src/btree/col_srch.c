/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(WT_TOC *toc, uint64_t recno, uint32_t level, uint32_t flags)
{
	DB *db;
	IDB *idb;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_REPL *repl;
	WT_RLE_EXPAND *exp;
	uint64_t record_cnt, start_recno;
	uint32_t base, i, indx, limit, write_gen;
	int ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;
	toc->srch_repl = NULL;
	toc->srch_exp = NULL;
	toc->srch_write_gen = 0;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db, "__wt_col_search", flags, WT_APIMASK_BT_SEARCH_COL);

	/* Search the tree. */
	for (page = idb->root_page.page;;) {
		/*
		 * Copy the page's write generation value before reading
		 * anything on the page.
		 */
		write_gen = page->write_gen;

		/* Walk the page looking for the record. */
		dsk = page->dsk;
		switch (dsk->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			cip = page->u.col_leaf.d + (recno - dsk->recno);
			goto done;
		case WT_PAGE_COL_RLE:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - dsk->recno;
			WT_COL_INDX_FOREACH(page, cip, i) {
				if (record_cnt < WT_RLE_REPEAT_COUNT(cip->data))
					break;
				record_cnt -= WT_RLE_REPEAT_COUNT(cip->data);
			}
			goto done;
		}

		/*
		 * Binary search of the page, looking for the right starting
		 * record.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			cref = page->u.col_int.t + indx;

			/*
			 * Like a row-store page, the 0th key sorts less than
			 * any application key.  Don't bother skipping the 0th
			 * index the way we do in the row-store binary search,
			 * key comparisons are cheap here.
			 */
			start_recno = WT_COL_REF_RECNO(cref);
			if (recno == start_recno)
				break;
			if (recno < start_recno)
				continue;
			base = indx + 1;
			--limit;
		}

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than recno and may be the
		 * 0th index or the (last + 1) indx.  If base is not the 0th
		 * index (remember, the 0th index always sorts less than any
		 * application recno), decrement it to the smallest index less
		 * than or equal to recno.
		 */
		if (recno != start_recno)
			cref = page->u.col_int.t + (base == 0 ? 0 : base - 1);

		/* If a level was set, see if we found the asked-for page. */
		if (level == dsk->level)
			goto done;

		/* cip references the subtree containing the record. */
		switch (ret =
		    __wt_page_in(toc, page, &cref->ref, cref->off_record, 0)) {
		case 0:				/* Valid page */
			/* Swap the parent page for the child page. */
			if (page != idb->root_page.page)
				__wt_hazard_clear(toc, page);
			break;
		case WT_PAGE_DELETED:
			/*
			 * !!!
			 * See __wt_rec_page_delete() for an explanation of page
			 * deletion.  As there are no real deletions of entries
			 * in column-store files, pages should never be deleted,
			 * and this shouldn't happen.
			 */
			goto notfound;
		default:
			goto err;
		}
		page = WT_COL_REF_PAGE(cref);
	}

done:	/*
	 * We've found the right on-page WT_COL structure, but that's only the
	 * first step; the record may have been updated since reading the page
	 * into the cache.
	 */
	switch (dsk->type) {
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
	case WT_PAGE_COL_RLE:
		/* Find the item's WT_COL_EXP slot if it exists. */
		for (exp =
		    WT_COL_RLEEXP(page, cip); exp != NULL; exp = exp->next)
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
			if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cip->data)))
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
