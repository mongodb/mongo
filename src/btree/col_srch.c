/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(SESSION *session, uint64_t recno, uint32_t flags)
{
	BTREE *btree;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_RLE_EXPAND *exp;
	WT_UPDATE *upd;
	uint64_t record_cnt, start_recno;
	uint32_t base, i, indx, limit, write_gen;
	int ret;
	void *cipdata;

	session->srch_page = NULL;			/* Return values. */
	session->srch_ip = NULL;
	session->srch_upd = NULL;
	session->srch_exp = NULL;
	session->srch_write_gen = 0;

	btree = session->btree;

	WT_DB_FCHK(btree, "__wt_col_search", flags, WT_APIMASK_BT_SEARCH_COL);

	/* Search the tree. */
	for (page = btree->root_page.page;;) {
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
			cipdata = WT_COL_PTR(dsk, cip);
			goto done;
		case WT_PAGE_COL_RLE:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - dsk->recno;
			WT_COL_INDX_FOREACH(page, cip, i) {
				cipdata = WT_COL_PTR(dsk, cip);
				if (record_cnt < WT_RLE_REPEAT_COUNT(cipdata))
					break;
				record_cnt -= WT_RLE_REPEAT_COUNT(cipdata);
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
			start_recno = cref->recno;
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

		/* cip references the subtree containing the record. */
		switch (ret = __wt_page_in(session, page, &cref->ref, 0)) {
		case 0:				/* Valid page */
			/* Swap the parent page for the child page. */
			if (page != btree->root_page.page)
				__wt_hazard_clear(session, page);
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
		/* Find the item's WT_UPDATE slot if it exists. */
		upd = WT_COL_UPDATE(page, cip);

		/*
		 * If overwriting an existing data item, we don't care if the
		 * item was previously deleted, return the gathered information.
		 */
		if (LF_ISSET(WT_DATA_OVERWRITE)) {
			session->srch_upd = upd;
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_UPDATE slot
		 * or in the original data.
		 */
		if (upd != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				goto notfound;
			session->srch_upd = upd;
		} else
			if (WT_FIX_DELETE_ISSET(cipdata))
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
				session->srch_exp = exp;
				session->srch_upd = exp->upd;
			}
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_UPDATE slot
		 * (referenced by the WT_COL_EXP slot), or in the original data.
		 */
		if (exp != NULL) {
			if (WT_UPDATE_DELETED_ISSET(exp->upd))
				goto notfound;
			session->srch_exp = exp;
			session->srch_upd = exp->upd;
		} else
			if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cipdata)))
				goto notfound;
		break;
	case WT_PAGE_COL_VAR:
		/* Find the item's WT_UPDATE slot if it exists. */
		upd = WT_COL_UPDATE(page, cip);

		/*
		 * If overwriting an existing data item, we don't care if the
		 * item was previously deleted, return the gathered information.
		 */
		if (LF_ISSET(WT_DATA_OVERWRITE)) {
			session->srch_upd = upd;
			break;
		}

		/*
		 * Otherwise, check for deletion, in either the WT_UPDATE slot
		 * or in the original data.
		 */
		if (upd != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				goto notfound;
			session->srch_upd = upd;
			break;
		} else
			if (WT_CELL_TYPE(cipdata) == WT_CELL_DEL)
				goto notfound;
		break;
	case WT_PAGE_COL_INT:
		/*
		 * When returning internal pages, set the item's WT_UPDATE slot
		 * if it exists, otherwise we're done.
		 */
		session->srch_upd = WT_COL_UPDATE(page, cip);
		break;
	WT_ILLEGAL_FORMAT(btree);
	}

	session->srch_page = page;
	session->srch_ip = cip;
	session->srch_write_gen = write_gen;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	WT_PAGE_OUT(session, page);
	return (ret);
}
