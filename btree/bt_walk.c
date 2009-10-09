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
__wt_bt_stat(WT_TOC *toc)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	int ret;

	fprintf(stderr, "stat not supported with multiple caches\n");
	return (0);

	db = toc->db;
	idb = db->idb;

	WT_STAT_INCR(idb->dstats, TREE_LEVEL, "number of levels in the Btree");

	/* If no root address has been set, it's a one-leaf-page database. */
	if (idb->root_addr == WT_ADDR_INVALID) {
		WT_RET(__wt_bt_page_in(toc, WT_ADDR_FIRST_PAGE, 1, 0, &page));
		ret = __wt_bt_stat_page(toc, page);
		WT_TRET(__wt_bt_page_out(toc, page, 0));
		return (ret);
	}

	return (__wt_bt_stat_level(toc, idb->root_addr, 0));
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
	ret = 0;
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
			if (hdr->type == WT_PAGE_INT ||
			    hdr->type == WT_PAGE_DUP_INT)
				__wt_bt_first_offp(
				    page, &addr_arg, &isleaf_arg);
		}

		WT_TRET(__wt_bt_page_out(toc, page, 0));
		if (ret != 0)
			return (ret);
	}

	if (addr_arg != WT_ADDR_INVALID) {
		WT_STAT_INCR(idb->dstats, TREE_LEVEL, NULL);
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
		WT_STAT_SET(idb->dstats,
		    FRAGSIZE, "database fragment size", db->allocsize);
		WT_STAT_SET(idb->dstats,
		    EXTSIZE, "database extent size", db->extsize);
	}

	/* Count the free space. */
	WT_STAT_INCRV(idb->dstats,
	    PAGE_FREE, "unused on-page space in bytes", page->space_avail);

	/* Count the page type. */
	switch (hdr->type) {
	case WT_PAGE_INT:
		WT_STAT_INCR(
		    idb->dstats, PAGE_INTERNAL, "primary internal pages");
		break;
	case WT_PAGE_DUP_INT:
		WT_STAT_INCR(
		    idb->dstats, PAGE_DUP_INTERNAL, "duplicate internal pages");
		break;
	case WT_PAGE_LEAF:
		WT_STAT_INCR(idb->dstats, PAGE_LEAF, "primary leaf pages");
		break;
	case WT_PAGE_DUP_LEAF:
		WT_STAT_INCR(idb->dstats, PAGE_DUP_LEAF, "duplicate leaf pages");
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(idb->dstats, PAGE_OVERFLOW, "overflow pages");
		break;
	WT_DEFAULT_FORMAT(db);
	}

	/* Count the items on leaf pages. */
	if (hdr->type != WT_PAGE_LEAF && hdr->type != WT_PAGE_DUP_LEAF)
		return (0);

	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(
			    idb->dstats, ITEM_KEY_OVFL, "overflow keys");
			break;
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(
			    idb->dstats, ITEM_DATA_OVFL, "overflow data items");
			break;
		default:
			break;
		}
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(
			    idb->dstats, ITEM_TOTAL_KEY, "total database keys");
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			WT_STAT_INCR(
			    idb->dstats, ITEM_DUP_DATA, "duplicate data items");
			/* FALLTHROUGH */
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(idb->dstats,
			    ITEM_TOTAL_DATA, "total database data items");
			break;
		case WT_ITEM_OFFP_INTL:
		case WT_ITEM_OFFP_LEAF:
			if (hdr->type != WT_PAGE_LEAF)
				break;
			WT_RET(__wt_bt_stat_level(toc,
			    ((WT_ITEM_OFFP *)WT_ITEM_BYTE(item))->addr,
			    WT_ITEM_TYPE(item) ==
			    WT_ITEM_OFFP_LEAF ? 1 : 0));
			break;
		default:
			break;
		}
	}

	return (0);
}
