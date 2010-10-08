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
 * __wt_bt_tree_walk --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_bt_tree_walk(WT_TOC *toc, u_int32_t addr,
    u_int32_t size, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE *page;
	WT_ROW *rip;
	u_int32_t i;
	int ret;

	db = toc->db;
	ret = 0;

	/*
	 * If we can't get the page, including if WT_RESTART is returned, fail
	 * because we don't know the source of the addr/size pair; our caller
	 * will have to deal with it.  (In almost all cases, however, our caller
	 * is a previous incarnation of this function -- which means our caller
	 * has a pinned page it's reading through.)
	 */
	WT_RET(__wt_bt_page_in(toc, addr, size, 1, &page));

	/* Call the worker function for the page. */
	WT_ERR(work(toc, page, arg));

	/*
	 * Walk any internal pages, descending through any off-page reference
	 * in the local tree (that is, NOT including off-page duplicate trees).
	 * We could handle off-page duplicate trees by walking the page in this
	 * function, but that would be slower than recursively calling this
	 * function from the worker function, which is already walking the page.
	 *
	 * If the page were to be rewritten/discarded from the cache while
	 * we're getting it, we can re-try -- re-trying is safe because our
	 * addr/size information is from a page which can't be discarded
	 * because of our hazard reference.  If the page was re-written, our
	 * on-page overflow information will have been updated to the overflow
	 * page's new address.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, cip, i)
			WT_ERR_RESTART(__wt_bt_tree_walk(toc,
			    WT_COL_OFF_ADDR(cip),
			    WT_COL_OFF_SIZE(cip), work, arg));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i)
			WT_ERR_RESTART(__wt_bt_tree_walk(toc,
			    WT_ROW_OFF_ADDR(rip),
			    WT_ROW_OFF_SIZE(rip), work, arg));
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
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
int
__wt_bt_stat_page(WT_TOC *toc, WT_PAGE *page, void *arg)
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
		case WT_ITEM_DEL:
			WT_STAT_INCR(idb->dstats, ITEM_TOTAL_DELETED);
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
			WT_STAT_INCR(idb->dstats, DUP_TREE);
			/*
			 * Recursively call the tree-walk function for the
			 * off-page duplicate tree.
			 */
			off = WT_ITEM_BYTE_OFF(item);
			WT_RET_RESTART(__wt_bt_tree_walk(toc,
			    off->addr, off->size, __wt_bt_stat_page, arg));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}

	return (0);
}
