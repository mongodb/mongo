/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_stat_page(WT_TOC *, WT_PAGE *);

/*
 * __wt_bt_stat --
 *	Depth-first recursive walk of a btree, calculating statistics.
 */
int
__wt_bt_stat(WT_TOC *toc, u_int32_t addr, u_int32_t size)
{
	DB *db;
	WT_COL_INDX *page_cip;
	WT_OFF *off;
	WT_PAGE *page;
	WT_ROW_INDX *page_rip;
	u_int32_t i;
	int ret;

	db = toc->db;
	ret = 0;

	WT_RET(__wt_bt_page_in(toc, addr, size, 1, &page));

	WT_ERR(__wt_bt_stat_page(toc, page));

	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, page_cip, i) {
			off = (WT_OFF *)page_cip->page_data;
			WT_ERR(__wt_bt_stat(toc, off->addr, off->size));
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, page_rip, i) {
			off = WT_ITEM_BYTE_OFF(page_rip->page_data);
			WT_ERR(__wt_bt_stat(toc, off->addr, off->size));
		}
		break;
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_bt_stat_page(toc, page));
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_bt_stat_page(toc, page));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

err: if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);

	return (ret);
}

/*
 * __wt_bt_stat_page --
 *	Stat a single Btree page.
 */
static int
__wt_bt_stat_page(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	IDB *idb;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	db = toc->db;
	idb = db->idb;
	hdr = page->hdr;

	/* Count the free space. */
	WT_STAT_INCRV(idb->dstats, PAGE_FREE, page->space_avail);

	/* Count the page type, and count items on leaf pages. */
	switch (hdr->type) {
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(idb->dstats, PAGE_COL_INTERNAL);
		return (0);
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(idb->dstats, PAGE_COL_FIXED);
		WT_STAT_INCRV(idb->dstats, ITEM_TOTAL_DATA, hdr->u.entries);
		return (0);
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(idb->dstats, PAGE_COL_VARIABLE);
		WT_STAT_INCRV(idb->dstats, ITEM_TOTAL_DATA, hdr->u.entries);
		return (0);
	case WT_PAGE_DUP_INT:
		WT_STAT_INCR(idb->dstats, PAGE_DUP_INTERNAL);
		return (0);
	case WT_PAGE_DUP_LEAF:
		WT_STAT_INCR(idb->dstats, PAGE_DUP_LEAF);
		break;
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(idb->dstats, PAGE_INTERNAL);
		return (0);
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(idb->dstats, PAGE_LEAF);
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(idb->dstats, PAGE_OVERFLOW);
		return (0);
	WT_ILLEGAL_FORMAT(db);
	}

	/* Row store leaf and duplicate leaf pages require counting. */
	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(idb->dstats, ITEM_KEY_OVFL);
			break;
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(idb->dstats, ITEM_DATA_OVFL);
			break;
		default:
			break;
		}
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(idb->dstats, ITEM_TOTAL_KEY);
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			WT_STAT_INCR(idb->dstats, ITEM_DUP_DATA);
			/* FALLTHROUGH */
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(idb->dstats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_OFF:
			WT_ASSERT(toc->env, hdr->type == WT_PAGE_ROW_LEAF);
			off = WT_ITEM_BYTE_OFF(item);
			WT_RET(__wt_bt_stat(toc, off->addr, off->size));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}

	return (0);
}
