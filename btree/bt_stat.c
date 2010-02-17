/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_stat_level(WT_TOC *, u_int32_t, int);
static int __wt_bt_stat_page(WT_TOC *, WT_PAGE *);

/*
 * __wt_bt_stat --
 *	Return Btree statistics.
 */
int
__wt_bt_stat(DB *db)
{
	ENV *env;
	IDB *idb;
	WT_TOC *toc;
	WT_PAGE *page;
	int ret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	WT_STAT_INCR(idb->dstats, TREE_LEVEL);

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.stat");

	/* If no root page has been set, there's nothing to stat. */
	if ((page = idb->root_page) == NULL)
		return (0);

	/* Check for one-page databases. */
	ret = page->hdr->type == WT_PAGE_ROW_LEAF ?
	    __wt_bt_stat_page(toc, page) :
	    __wt_bt_stat_level(toc, page->addr, 0);

	WT_TRET(toc->close(toc, 0));

	return (ret);
}

/*
 * __wt_bt_stat_level --
 *	Stat a level of a tree.
 */
static int
__wt_bt_stat_level(WT_TOC *toc, u_int32_t addr, int isleaf)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int32_t addr_arg;
	int first, isleaf_arg, ret;

	db = toc->db;
	idb = db->idb;
	isleaf_arg = ret = 0;
	addr_arg = WT_ADDR_INVALID;

	for (first = 1; addr != WT_ADDR_INVALID;) {
		/* Get the next page and stat it. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 0, &page));

		ret = __wt_bt_stat_page(toc, page);

		/*
		 * If we're walking an internal page, we'll want to descend
		 * to the first offpage in this level, save the address and
		 * level information for the next iteration.
		 */
		hdr = page->hdr;
		addr = hdr->nextaddr;
		if (first) {
			first = 0;
			switch (hdr->type) {
			case WT_PAGE_COL_INT:
			case WT_PAGE_DUP_INT:
			case WT_PAGE_ROW_INT:
				__wt_bt_first_offp(
				    page, &addr_arg, &isleaf_arg);
				break;
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_VAR:
			case WT_PAGE_DUP_LEAF:
			case WT_PAGE_ROW_LEAF:
				break;
			WT_ILLEGAL_FORMAT(db);
			}
		}

		WT_TRET(__wt_bt_page_out(toc, page, 0));
		if (ret != 0)
			return (ret);
	}

	if (addr_arg != WT_ADDR_INVALID) {
		WT_STAT_INCR(idb->dstats, TREE_LEVEL);
		ret = __wt_bt_stat_level(toc, addr_arg, isleaf_arg);
	}

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
	WT_PAGE_HDR *hdr;
	u_int32_t addr, i;

	db = toc->db;
	idb = db->idb;
	hdr = page->hdr;
	addr = page->addr;

	/* Page 0 has the descriptor record, get all-database statistics. */
	if (addr == WT_ADDR_FIRST_PAGE) {
		__wt_bt_desc_stats(db, page);
		WT_STAT_SET(idb->dstats, FRAGSIZE, db->allocsize);
		WT_STAT_SET(idb->dstats, EXTSIZE, db->extsize);
	}

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
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			WT_ASSERT(toc->env, hdr->type == WT_PAGE_ROW_LEAF);
			WT_RET(__wt_bt_stat_level(toc,
			    WT_ITEM_BYTE_OFF(item)->addr,
			    WT_ITEM_TYPE(item) == WT_ITEM_OFF_LEAF ? 1 : 0));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}

	return (0);
}
