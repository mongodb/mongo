/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

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
	BTREE *btree;
	WT_STATS *stats;

	btree = session->btree;
	stats = btree->fstats;

	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(stats, PAGE_COL_FIX);
		WT_RET(__wt_stat_page_col_fix(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(stats, PAGE_COL_INTERNAL);
		break;
	case WT_PAGE_COL_RLE:
		WT_STAT_INCR(stats, PAGE_COL_RLE);
		WT_RET(__wt_stat_page_col_rle(session, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(stats, PAGE_COL_VARIABLE);
		WT_RET(__wt_stat_page_col_var(session, page));
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(stats, PAGE_OVERFLOW);
		break;
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(stats, PAGE_ROW_INTERNAL);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(stats, PAGE_ROW_LEAF);
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
	WT_COL *cip;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	stats = session->btree->fstats;

	/* Walk the page, counting data items. */
	WT_COL_INDX_FOREACH(page, cip, i)
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL)
			if (WT_FIX_DELETE_ISSET(WT_COL_PTR(page, cip)))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
		else
			if (WT_UPDATE_DELETED_ISSET(upd))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
	return (0);
}

/*
 * __wt_stat_page_col_rle --
 *	Stat a WT_PAGE_COL_RLE page.
 */
static int
__wt_stat_page_col_rle(SESSION *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_INSERT *ins;
	WT_STATS *stats;
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
			    ITEM_COL_DELETED, WT_RLE_REPEAT_COUNT(cipdata));
			orig_deleted = 1;
		} else {
			WT_STAT_INCRV(stats,
			    ITEM_TOTAL_DATA, WT_RLE_REPEAT_COUNT(cipdata));
			orig_deleted = 0;
		}

		/* Walk the insert list, checking for changes. */
		for (ins =
		    WT_COL_INSERT(page, cip); ins != NULL; ins = ins->next) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (orig_deleted)
					continue;
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
				WT_STAT_DECR(stats, ITEM_TOTAL_DATA);
			} else {
				if (!orig_deleted)
					continue;
				WT_STAT_DECR(stats, ITEM_COL_DELETED);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
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
	BTREE *btree;
	WT_COL *cip;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	btree = session->btree;
	stats = btree->fstats;

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
			upd = WT_COL_UPDATE(page, cip);
			if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_CELL_DATA_OVFL:
			upd = WT_COL_UPDATE(page, cip);
			if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd)) {
				WT_STAT_INCR(stats, ITEM_DATA_OVFL);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
			break;
		case WT_CELL_DEL:
			WT_STAT_INCR(stats, ITEM_COL_DELETED);
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
	BTREE *btree;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	WT_UNUSED(arg);
	btree = session->btree;
	stats = btree->fstats;

	/*
	 * Stat any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	for (ins = WT_ROW_INSERT_SMALLEST(page); ins != NULL; ins = ins->next)
		if (!WT_UPDATE_DELETED_ISSET(ins->upd)) {
			WT_STAT_INCR(stats, ITEM_TOTAL_KEY);
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
		}

	/*
	 * Stat the page's K/V pairs.
	 *
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any updates weren't deletions.  If the item was updated,
	 * assume it was updated by an item of the same size (it's expensive to
	 * figure out if it will require the same space or not, especially if
	 * there's Huffman encoding).
	 */
	WT_ROW_INDX_FOREACH(page, rip, i) {
		if (!WT_ROW_EMPTY_ISSET(rip))
			switch (WT_CELL_TYPE(WT_ROW_PTR(page, rip))) {
			case WT_CELL_DATA:
				upd = WT_ROW_UPDATE(page, rip);
				if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
					goto stat_insert;
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
				break;
			case WT_CELL_DATA_OVFL:
				upd = WT_ROW_UPDATE(page, rip);
				if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
					goto stat_insert;
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
				WT_STAT_INCR(stats, ITEM_DATA_OVFL);
				break;
			}

		/*
		 * If the data item wasn't deleted, count the key.
		 *
		 * If we have processed the key, we have lost the information as
		 * to whether or not it's an overflow key.
		 */
		WT_STAT_INCR(stats, ITEM_TOTAL_KEY);
		if (__wt_key_process(rip) &&
		    WT_CELL_TYPE(rip->key) == WT_CELL_KEY_OVFL)
			WT_STAT_INCR(stats, ITEM_KEY_OVFL);

stat_insert:	/* Stat inserted K/V pairs. */
		for (ins =
		    WT_ROW_INSERT(page, rip); ins != NULL; ins = ins->next)
			if (!WT_UPDATE_DELETED_ISSET(ins->upd)) {
				WT_STAT_INCR(stats, ITEM_TOTAL_KEY);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
	}
	return (0);
}
