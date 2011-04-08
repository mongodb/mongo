/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_stat_page_col_fix(SESSION *, WT_PAGE *);
static int __wt_stat_page_col_rle(SESSION *, WT_PAGE *);
static int __wt_stat_page_col_var(SESSION *, WT_PAGE *);
static int __wt_stat_page_row_leaf(SESSION *, WT_PAGE *, void *);

/*
 * __wt_page_stat --
 *	Stat any Btree page.
 */
int
__wt_page_stat(SESSION *session, WT_PAGE *page, void *arg)
{
	WT_BTREE_FILE_STATS *stats;

	stats = session->btree->fstats;

	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(stats, file_col_fix);
		WT_RET(__wt_stat_page_col_fix(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(stats, file_col_internal);
		break;
	case WT_PAGE_COL_RLE:
		WT_STAT_INCR(stats, file_col_rle);
		WT_RET(__wt_stat_page_col_rle(session, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(stats, file_col_variable);
		WT_RET(__wt_stat_page_col_var(session, page));
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(stats, file_overflow);
		break;
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(stats, file_row_internal);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(stats, file_row_leaf);
		WT_RET(__wt_stat_page_row_leaf(session, page, arg));
		break;
	WT_ILLEGAL_FORMAT(session);
	}
	return (0);
}

/*
 * __wt_stat_page_col_fix --
 *	Stat a WT_PAGE_COL_FIX page.
 */
static int
__wt_stat_page_col_fix(SESSION *session, WT_PAGE *page)
{
	WT_BTREE_FILE_STATS *stats;
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;

	stats = session->btree->fstats;

	/* Walk the page, counting data items. */
	WT_COL_INDX_FOREACH(page, cip, i)
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL)
			if (WT_FIX_DELETE_ISSET(WT_COL_PTR(page, cip)))
				WT_STAT_INCR(stats, file_item_col_deleted);
			else
				WT_STAT_INCR(stats, file_item_total_data);
		else
			if (WT_UPDATE_DELETED_ISSET(upd))
				WT_STAT_INCR(stats, file_item_col_deleted);
			else
				WT_STAT_INCR(stats, file_item_total_data);
	return (0);
}

/*
 * __wt_stat_page_col_rle --
 *	Stat a WT_PAGE_COL_RLE page.
 */
static int
__wt_stat_page_col_rle(SESSION *session, WT_PAGE *page)
{
	WT_BTREE_FILE_STATS *stats;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint32_t i;
	int orig_deleted;
	void *cipdata;

	stats = session->btree->fstats;

	/* Walk the page, counting data items. */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cipdata))) {
			WT_STAT_INCRV(stats,
			    file_item_col_deleted,
			    WT_RLE_REPEAT_COUNT(cipdata));
			orig_deleted = 1;
		} else {
			WT_STAT_INCRV(stats,
			    file_item_total_data, WT_RLE_REPEAT_COUNT(cipdata));
			orig_deleted = 0;
		}

		/*
		 * Walk the insert list, checking for changes.  For each insert
		 * we find, correct the original count based on its state.
		 */
		for (ins =
		    WT_COL_INSERT(page, cip); ins != NULL; ins = ins->next) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (orig_deleted)
					continue;
				WT_STAT_INCR(stats, file_item_col_deleted);
				WT_STAT_DECR(stats, file_item_total_data);
			} else {
				if (!orig_deleted)
					continue;
				WT_STAT_DECR(stats, file_item_col_deleted);
				WT_STAT_INCR(stats, file_item_total_data);
			}
		}
	}
	return (0);
}

/*
 * __wt_stat_page_col_var --
 *	Stat a WT_PAGE_COL_VAR page.
 */
static int
__wt_stat_page_col_var(SESSION *session, WT_PAGE *page)
{
	WT_BTREE_FILE_STATS *stats;
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;

	stats = session->btree->fstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any updates weren't deletions.  If the item was updated,
	 * assume it was updated by an item of the same size (it's expensive to
	 * figure out if it will require the same space or not, especially if
	 * there's Huffman encoding).
	 */
	WT_COL_INDX_FOREACH(page, cip, i)
		switch (WT_CELL_TYPE(WT_COL_PTR(page, cip))) {
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
			upd = WT_COL_UPDATE(page, cip);
			if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd)) {
				WT_STAT_INCR(stats, file_item_total_data);
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_DEL:
			WT_STAT_INCR(stats, file_item_col_deleted);
			break;
		}
	return (0);
}

/*
 * __wt_stat_page_row_leaf --
 *	Stat a WT_PAGE_ROW_LEAF page.
 */
static int
__wt_stat_page_row_leaf(SESSION *session, WT_PAGE *page, void *arg)
{
	WT_BTREE_FILE_STATS *stats;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t cnt, i;

	WT_UNUSED(arg);
	stats = session->btree->fstats;

	/*
	 * Stat any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	cnt = 0;
	for (ins = WT_ROW_INSERT_SMALLEST(page); ins != NULL; ins = ins->next)
		if (!WT_UPDATE_DELETED_ISSET(ins->upd))
			++cnt;

	/* Stat the page's K/V pairs. */
	WT_ROW_INDX_FOREACH(page, rip, i) {
		upd = WT_ROW_UPDATE(page, rip);
		if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
			++cnt;

		/* Stat inserted K/V pairs. */
		for (ins =
		    WT_ROW_INSERT(page, rip); ins != NULL; ins = ins->next)
			if (!WT_UPDATE_DELETED_ISSET(ins->upd))
				++cnt;
	}

	WT_STAT_INCRV(stats, file_item_total_key, cnt);
	WT_STAT_INCRV(stats, file_item_total_data, cnt);

	return (0);
}
