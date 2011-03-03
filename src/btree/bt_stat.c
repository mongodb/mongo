/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_stat_page_col_fix(WT_TOC *, WT_PAGE *);
static int __wt_stat_page_col_rle(WT_TOC *, WT_PAGE *);
static int __wt_stat_page_col_var(WT_TOC *, WT_PAGE *);
static int __wt_stat_page_row_leaf(WT_TOC *, WT_PAGE *, void *);

/*
 * __wt_page_stat --
 *	Stat any Btree page.
 */
int
__wt_page_stat(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_STATS *stats;

	db = toc->db;
	btree = db->btree;
	dsk = page->dsk;
	stats = btree->fstats;

	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(stats, PAGE_COL_FIX);
		WT_RET(__wt_stat_page_col_fix(toc, page));
		break;
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(stats, PAGE_COL_INTERNAL);
		break;
	case WT_PAGE_COL_RLE:
		WT_STAT_INCR(stats, PAGE_COL_RLE);
		WT_RET(__wt_stat_page_col_rle(toc, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(stats, PAGE_COL_VARIABLE);
		WT_RET(__wt_stat_page_col_var(toc, page));
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(stats, PAGE_OVERFLOW);
		break;
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(stats, PAGE_ROW_INTERNAL);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(stats, PAGE_ROW_LEAF);
		WT_RET(__wt_stat_page_row_leaf(toc, page, arg));
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (0);
}

/*
 * __wt_stat_page_col_fix --
 *	Stat a WT_PAGE_COL_FIX page.
 */
static int
__wt_stat_page_col_fix(WT_TOC *toc, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	dsk = page->dsk;
	stats = toc->db->btree->fstats;

	/* Walk the page, counting data items. */
	WT_COL_INDX_FOREACH(page, cip, i)
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL)
			if (WT_FIX_DELETE_ISSET(WT_COL_PTR(dsk, cip)))
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
__wt_stat_page_col_rle(WT_TOC *toc, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_RLE_EXPAND *exp;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;
	void *cipdata;

	dsk = page->dsk;
	stats = toc->db->btree->fstats;

	/* Walk the page, counting data items. */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(dsk, cip);
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cipdata)))
			WT_STAT_INCRV(stats,
			    ITEM_COL_DELETED, WT_RLE_REPEAT_COUNT(cipdata));
		else
			WT_STAT_INCRV(stats,
			    ITEM_TOTAL_DATA, WT_RLE_REPEAT_COUNT(cipdata));

		/*
		 * Check for corrections.
		 *
		 * XXX
		 * This gets the count wrong if an application changes existing
		 * records, or updates a deleted record two times in a row --
		 * we'll incorrectly count the records as unique, when they are
		 * changes to the same record.  I'm not fixing it as I don't
		 * expect the WT_COL_RLEEXP data structure to be permanent, it's
		 * too likely to become a linked list in bad cases.
		 */
		for (exp =
		    WT_COL_RLEEXP(page, cip); exp != NULL; exp = exp->next) {
			upd = exp->upd;
			if (WT_UPDATE_DELETED_ISSET(upd))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
		}
	}
	return (0);
}

/*
 * __wt_stat_page_col_var --
 *	Stat a WT_PAGE_COL_VAR page.
 */
static int
__wt_stat_page_col_var(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	db = toc->db;
	dsk = page->dsk;
	stats = db->btree->fstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any updates weren't deletions.  If the item was updated,
	 * assume it was updated by an item of the same size (it's expensive to
	 * figure out if it will require the same space or not, especially if
	 * there's Huffman encoding).
	 */
	WT_COL_INDX_FOREACH(page, cip, i) {
		switch (WT_ITEM_TYPE(WT_COL_PTR(dsk, cip))) {
		case WT_ITEM_DATA:
			upd = WT_COL_UPDATE(page, cip);
			if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_OVFL:
			upd = WT_COL_UPDATE(page, cip);
			if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd)) {
				WT_STAT_INCR(stats, ITEM_DATA_OVFL);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
			break;
		case WT_ITEM_DEL:
			WT_STAT_INCR(stats, ITEM_COL_DELETED);
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}
	return (0);
}

/*
 * __wt_stat_page_row_leaf --
 *	Stat a WT_PAGE_ROW_LEAF page.
 */
static int
__wt_stat_page_row_leaf(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	WT_ROW *rip;
	WT_STATS *stats;
	WT_UPDATE *upd;
	uint32_t i;

	arg = NULL;				/* Shut the compiler up. */
	db = toc->db;
	stats = db->btree->fstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any updates weren't deletions.  If the item was updated,
	 * assume it was updated by an item of the same size (it's expensive to
	 * figure out if it will require the same space or not, especially if
	 * there's Huffman encoding).
	 */
	WT_ROW_INDX_FOREACH(page, rip, i) {
		switch (WT_ITEM_TYPE(rip->value)) {
		case WT_ITEM_DATA:
			upd = WT_ROW_UPDATE(page, rip);
			if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
				continue;
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_OVFL:
			upd = WT_ROW_UPDATE(page, rip);
			if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
				continue;
			WT_STAT_INCR(stats, ITEM_DATA_OVFL);
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		/*
		 * If the data item wasn't deleted, count the key.
		 *
		 * If we have processed the key, we have lost the information as
		 * to whether or not it's an overflow key -- we can figure out
		 * if it's Huffman encoded by looking at the huffman key, but
		 * that doesn't tell us if it's an overflow key or not.  To fix
		 * this we'd have to maintain a reference to the on-page key and
		 * check it, and I'm not willing to spend the additional pointer
		 * in the WT_ROW structure.
		 */
		if (__wt_key_process(rip))
			switch (WT_ITEM_TYPE(rip->key)) {
			case WT_ITEM_KEY_OVFL:
				WT_STAT_INCR(stats, ITEM_KEY_OVFL);
				/* FALLTHROUGH */
			case WT_ITEM_KEY:
				WT_STAT_INCR(stats, ITEM_TOTAL_KEY);
				break;
			WT_ILLEGAL_FORMAT(db);
			}
		else
			WT_STAT_INCR(stats, ITEM_TOTAL_KEY);

	}
	return (0);
}
