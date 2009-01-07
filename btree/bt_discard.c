/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_int(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_leaf(DB *, WT_PAGE *);

/*
 * WT_SET_FF_AND_SA_FROM_ADDR --
 *	Set the page's first-free and space-available values from an
 *	address positioned one past the last used byte on the page.
 *	Common to functions in this file.
 */
#define	WT_SET_FF_AND_SA_FROM_ADDR(db, page, item)			\
	(page)->first_free = (u_int8_t *)(item);			\
	(page)->space_avail = (db)->pagesize -				\
	    (u_int32_t)((page)->first_free - (u_int8_t *)(page)->hdr);

/*
 * __wt_bt_page_inmem --
 *	Initialize the in-memory page structure after a page is read.
 */
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_INDX *indx;
	WT_PAGE_HDR *hdr;
	int ret;

	env = db->env;
	hdr = page->hdr;

	/* Build page indexes for all page types other than overflow pages. */
	if (hdr->type == WT_PAGE_OVFL)
		return (0);

	if ((ret =
	    __wt_calloc(env, hdr->u.entries, sizeof(WT_INDX), &indx)) != 0)
		return (ret);
	page->indx = indx;
	page->indx_size = hdr->u.entries;

	switch (hdr->type) {
	case WT_PAGE_INT:
	case WT_PAGE_DUP_INT:
		ret = __wt_bt_page_inmem_int(db, page);
		break;
	case WT_PAGE_LEAF:
		ret = __wt_bt_page_inmem_leaf(db, page);
		break;
	case WT_PAGE_DUP_LEAF:
		ret = __wt_bt_page_inmem_dup_leaf(db, page);
		break;
	default:
		return (__wt_database_format(db));
	}
	return (ret);
}

/*
 * __wt_bt_page_inmem_int --
 *	Build in-memory index for primary and off-page duplicate tree internal
 *	pages.
 */
static int
__wt_bt_page_inmem_int(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OFFP *offp;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;
	indx = page->indx;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted key/offpage-reference pairs.  Keys
	 *	are on-page (WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.
	 *	Offpage references are WT_ITEM_OFFPAGE items.
	 */
	WT_ITEM_FOREACH(page, item, i)
		switch (item->type) {
		case WT_ITEM_KEY:
			indx->data = WT_ITEM_BYTE(item);
			indx->size = item->len;
			indx->addr = WT_ADDR_INVALID;
			break;
		case WT_ITEM_KEY_OVFL:
			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			indx->addr = ovfl->addr;
			break;
		case WT_ITEM_OFFPAGE:
			offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
			indx->addr = offp->addr;
			indx->ditem = item;
			++indx;
			break;
		default:
			return (__wt_database_format(db));
		}

	page->indx_count = hdr->u.entries / 2;
	WT_SET_FF_AND_SA_FROM_ADDR(db, page, item);

	return (0);
}

/*
 * __wt_bt_page_inmem_leaf --
 *	Build in-memory index for primary leaf pages.
 */
static int
__wt_bt_page_inmem_leaf(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	u_int32_t i;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted key/data sets.  Keys are on-page
	 *	(WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.  The data
	 *	sets are either: a single on-page (WT_ITEM_DATA) or overflow
	 *	(WT_ITEM_DATA_OVFL) item; a group of duplicate data items
	 *	where each duplicate is an on-page (WT_ITEM_DUP) or overflow
	 *	(WT_ITEM_DUP_OVFL) item; an offpage reference (WT_ITEM_OFFPAGE).
	 */
	indx = NULL;
	WT_ITEM_FOREACH(page, item, i)
		switch (item->type) {
		case WT_ITEM_KEY:
			if (indx == NULL)
				indx = page->indx;
			else
				++indx;

			indx->data = WT_ITEM_BYTE(item);
			indx->size = item->len;
			indx->addr = WT_ADDR_INVALID;

			++page->indx_count;
			break;
		case WT_ITEM_KEY_OVFL:
			if (indx == NULL)
				indx = page->indx;
			else
				++indx;

			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			indx->addr = ovfl->addr;

			++page->indx_count;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_OFFPAGE:
			if (indx->ditem == NULL)
				indx->ditem = item;
			break;
		default:
			return (__wt_database_format(db));
		}

	WT_SET_FF_AND_SA_FROM_ADDR(db, page, item);
	return (0);
}

/*
 * __wt_bt_page_inmem_dup_leaf --
 *	Build in-memory index for off-page duplicate tree leaf pages.
 */
static int
__wt_bt_page_inmem_dup_leaf(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted data items.  The data items are
	 *	on-page (WT_ITEM_DUP) or overflow (WT_ITEM_DUP_OVFL).
	 */
	indx = page->indx;
	WT_ITEM_FOREACH(page, item, i) {
		switch (item->type) {
		case WT_ITEM_DUP:
			indx->data = WT_ITEM_BYTE(item);
			indx->size = item->len;
			indx->addr = WT_ADDR_INVALID;
			break;
		case WT_ITEM_DATA_OVFL:
			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			indx->addr = ovfl->addr;
			break;
		default:
			return (__wt_database_format(db));
		}
		indx->ditem = item;
		++indx;
	}

	page->indx_count = hdr->u.entries;
	WT_SET_FF_AND_SA_FROM_ADDR(db, page, item);

	return (0);
}

/*
 * __wt_bt_page_inmem_alloc --
 *	Initialize the in-memory page structure after a page is allocated.
 */
void
__wt_bt_page_inmem_alloc(DB *db, WT_PAGE *page)
{
	WT_SET_FF_AND_SA_FROM_ADDR(db, page, WT_PAGE_BYTE(page));

	if (page->addr == 0)
		__wt_bt_desc_init(db, page);
}
