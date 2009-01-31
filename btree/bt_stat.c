/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_stat_level(DB *, WT_ITEM_OFFP *);
static int __wt_bt_stat_page(DB *, WT_PAGE *);

/*
 * __wt_bt_stat --
 *	Return Btree statistics.
 */
int
__wt_bt_stat(DB *db)
{
	IDB *idb;
	WT_PAGE *page;
	WT_ITEM_OFFP offp;
	int ret, tret;

	idb = db->idb;

	/* If no root address has been set, it's a one-leaf-page database. */
	if (idb->root_addr == WT_ADDR_INVALID) {
		if ((ret =
		    __wt_bt_page_in(db, WT_ADDR_FIRST_PAGE, 1, &page)) != 0)
			return (ret);
		ret = __wt_bt_stat_page(db, page);
		if ((tret = __wt_bt_page_out(db, page, 0)) != 0 && ret == 0)
			ret = tret;
		return (ret);
	}

	/*
	 * Construct an OFFP for __wt_bt_stat_level -- the addr is correct,
	 * but the level is not.   We don't store the level in the DESC
	 * structure, so there's no way to know what the correct level is yet.
	 */
	offp.addr = idb->root_addr;
	offp.level = WT_FIRST_INTERNAL_LEVEL;
	return (__wt_bt_stat_level(db, &offp));
}

/*
 * __wt_bt_stat_level --
 *	Stat a level of a tree.
 */
static int
__wt_bt_stat_level(DB *db, WT_ITEM_OFFP *offp)
{
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int first, isleaf, ret, tret;

	ret = 0;

	isleaf = offp->level == WT_LEAF_LEVEL ? 1 : 0;
	for (first = 1, addr = offp->addr; addr != WT_ADDR_INVALID;) {
		/* Get the next page and stat it. */
		if ((ret = __wt_bt_page_in(db, addr, isleaf, &page)) != 0)
			return (ret);

		ret = __wt_bt_stat_page(db, page);

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
				__wt_bt_first_offp(page, offp);
			else
				offp = NULL;
		}

		if ((tret = __wt_bt_page_out(db, page, 0)) != 0 && ret == 0) {
			ret = tret;
			return (ret);
		}
	}

	if (offp != NULL)
		ret = __wt_bt_stat_level(db, offp);

	return (ret);
}

/*
 * __wt_bt_stat_page --
 *	Stat a single Btree page.
 */
static int
__wt_bt_stat_page(DB *db, WT_PAGE *page)
{
	WT_ITEM *item;
	WT_ITEM_OFFP offp;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags, i;
	int ret;

	hdr = page->hdr;
	addr = page->addr;

	/* Page 0 has the descriptor record, get all-database statistics. */
	if (addr == WT_ADDR_FIRST_PAGE) {
		__wt_bt_desc_stats(db, page);
		WT_STAT_SET(db->dstats,
		    FRAGSIZE, "database fragment size", db->allocsize);
		WT_STAT_SET(db->dstats,
		    EXTSIZE, "database extent size", db->extsize);
	}

	/* Count the free space. */
	WT_STAT_INCRV(db->dstats,
	    PAGE_FREE, "unused on-page space in bytes", page->space_avail);

	/* Count the page type. */
	switch (hdr->type) {
	case WT_PAGE_INT:
		WT_STAT_INCR(
		    db->dstats, PAGE_INTERNAL, "primary internal pages");
		break;
	case WT_PAGE_DUP_INT:
		WT_STAT_INCR(
		    db->dstats, PAGE_DUP_INTERNAL, "duplicate internal pages");
		break;
	case WT_PAGE_LEAF:
		WT_STAT_INCR(db->dstats, PAGE_LEAF, "primary leaf pages");
		break;
	case WT_PAGE_DUP_LEAF:
		WT_STAT_INCR(db->dstats, PAGE_DUP_LEAF, "duplicate leaf pages");
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(db->dstats, PAGE_OVERFLOW, "overflow pages");
		break;
	WT_DEFAULT_FORMAT(db);
	}

	/* Count the items on leaf pages. */
	if (hdr->type != WT_PAGE_LEAF && hdr->type != WT_PAGE_DUP_LEAF)
		return (0);

	WT_ITEM_FOREACH(page, item, i) {
		switch (item->type) {
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(
			    db->dstats, ITEM_KEY_OVFL, "overflow keys");
			break;
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(
			    db->dstats, ITEM_DATA_OVFL, "overflow data items");
			break;
		}
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			WT_STAT_INCR(
			    db->dstats, ITEM_TOTAL_KEY, "total database keys");
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			WT_STAT_INCR(
			    db->dstats, ITEM_DUP_DATA, "duplicate data items");
			/* FALLTHROUGH */
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			WT_STAT_INCR(db->dstats,
			    ITEM_TOTAL_DATA, "total database data items");
			break;
		case WT_ITEM_OFFPAGE:
			if (hdr->type != WT_PAGE_LEAF)
				break;
			/*
			 * !!!
			 * Don't pass __wt_bt_stat_level a pointer
			 * to the on-page OFFP structure, it writes
			 * the offp passed in.
			 */
			offp = *(WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
			if ((ret = __wt_bt_stat_level(db, &offp)) != 0)
				return (ret);
			break;
		}
	}

	return (0);
}
