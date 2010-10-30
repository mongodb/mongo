/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_page_inmem_col_fix(DB *, WT_PAGE *);
static void __wt_bt_page_inmem_col_int(WT_PAGE *);
static void __wt_bt_page_inmem_col_rcc(DB *, WT_PAGE *);
static void __wt_bt_page_inmem_col_var(WT_PAGE *);
static int  __wt_bt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_row_int(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_bt_page_alloc --
 *	Allocate a new btree page from the cache.
 */
int
__wt_bt_page_alloc(
    WT_TOC *toc, u_int type, u_int level, u_int32_t size, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;

	WT_RET((__wt_page_alloc(toc, size, &page)));

	/* In-memory page structure: the space-available and first-free byte. */
	__wt_bt_set_ff_and_sa_from_offset(page, WT_PAGE_BYTE(page));

	/*
	 * Generally, the default values of 0 on page are correct; set the type
	 * and the level.
	 */
	hdr = page->hdr;
	hdr->type = (u_int8_t)type;
	hdr->level = (u_int8_t)level;

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_page_in --
 *	Get a btree page from the cache.
 */
int
__wt_bt_page_in(
    WT_TOC *toc, u_int32_t addr, u_int32_t size, int inmem, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	/*
	 * We don't know the source of the addr/size pair, so we pass back any
	 * WT_RESTART failures; the caller has to handle them.
	 */
	WT_RET(__wt_page_in(toc, addr, size, &page, 0));

	/* Optionally build the in-memory version of the page. */
	if (inmem && !WT_PAGE_INMEM_SET(page))
		WT_RET((__wt_bt_page_inmem(db, page)));

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_page_out --
 *	Return a btree page to the cache.
 */
void
__wt_bt_page_out(WT_TOC *toc, WT_PAGE **pagep, u_int32_t flags)
{
	WT_PAGE *page;

	WT_ENV_FCHK_ASSERT(
	    toc->env, "__wt_bt_page_out", flags, WT_APIMASK_BT_PAGE_OUT);

	/*
	 * Clear the caller's reference so we don't accidentally use a page
	 * after discarding our reference, and to make it easy to decide if
	 * a page is in-use after our return.
	 */
	page = *pagep;
	*pagep = NULL;

	/* The caller may have decided the page isn't worth keeping around. */
	if (LF_ISSET(WT_DISCARD))
		page->lru = 0;

	/* The caller may have dirtied the page. */
	if (LF_ISSET(WT_MODIFIED))
		WT_PAGE_MODIFY_SET(page);

	__wt_page_out(toc, page);
}

/*
 * __wt_bt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE_HDR *hdr;
	u_int32_t nindx;
	int ret;

	env = db->env;
	hdr = page->hdr;
	ret = 0;

	WT_ASSERT(env, page->u.indx == NULL);

	/* Determine the maximum number of indexes we'll need for this page. */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
		nindx = hdr->u.entries;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		nindx = hdr->u.entries / 2;
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Row store leaf pages support duplicates, so the real worst
		 * case is one key plus some number of duplicate data items.
		 * The number is configurable, that is, you can configure when
		 * a duplicate set is big enough to be pushed off the page;
		 * we're conservative here.
		 */
		nindx = hdr->u.entries - 1;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * XXX
	 * We don't yet have a free-list on which to put empty pages -- for
	 * now, we handle them.
	 */
	if (nindx == 0)
		return (0);

	/* Allocate an array of WT_{ROW,COL}_INDX structures for the page. */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
		WT_RET((__wt_calloc(env,
		    nindx, sizeof(WT_COL), &page->u.icol)));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET((__wt_calloc(env,
		    nindx, sizeof(WT_ROW), &page->u.irow)));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		__wt_bt_page_inmem_col_fix(db, page);
		break;
	case WT_PAGE_COL_INT:
		__wt_bt_page_inmem_col_int(page);
		break;
	case WT_PAGE_COL_RCC:
		__wt_bt_page_inmem_col_rcc(db, page);
		break;
	case WT_PAGE_COL_VAR:
		__wt_bt_page_inmem_col_var(page);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		ret = __wt_bt_page_inmem_row_int(db, page);
		break;
	case WT_PAGE_DUP_LEAF:
		ret = __wt_bt_page_inmem_dup_leaf(db, page);
		break;
	case WT_PAGE_ROW_LEAF:
		ret = __wt_bt_page_inmem_row_leaf(db, page);
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (ret);
}

/*
 * __wt_bt_page_inmem_row_int --
 *	Build in-memory index for row-store and off-page duplicate tree
 *	internal pages.
 */
static int
__wt_bt_page_inmem_row_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	u_int64_t records;
	u_int32_t i;
	void *huffman;

	idb = db->idb;
	hdr = page->hdr;
	rip = page->u.irow;
	records = 0;

	huffman =
	    hdr->type == WT_PAGE_DUP_INT ? idb->huffman_data : idb->huffman_key;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 * The page contains sorted key/offpage-reference pairs.  Keys are row
	 * store internal pages with on-page/overflow (WT_ITEM_KEY/KEY_OVFL)
	 * items, or row store duplicate internal pages with on-page/overflow
	 * (WT_ITEM_KEY_DUP/WT_ITEM_DATA_KEY_DUP_OVFL) items.  In both cases,
	 * offpage references are WT_ITEM_OFF items.
	 */
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
			if (huffman == NULL) {
				WT_KEY_SET(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
			WT_KEY_SET_PROCESS(rip, item);
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			records += WT_RECORDS(off);
			rip->data = item;
			++rip;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = hdr->u.entries / 2;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_bt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_ROW *rip;
	u_int32_t i, indx_count;
	u_int64_t records;

	idb = db->idb;
	records = 0;

	/*
	 * Walk a row-store page of WT_ITEMs, building indices and finding the
	 * end of the page.
	 *
	 * The page contains key/data pairs.  Keys are on-page (WT_ITEM_KEY) or
	 * overflow (WT_ITEM_KEY_OVFL) items.  The data sets are either: a
	 * single on-page (WT_ITEM_DATA) or overflow (WT_ITEM_DATA_OVFL) item;
	 * a group of duplicate data items where each duplicate is an on-page
	 * (WT_ITEM_DATA_DUP) or overflow (WT_ITEM_DUP_OVFL) item; or an offpage
	 * reference (WT_ITEM_OFF).
	 */
	rip = NULL;
	indx_count = 0;
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (rip == NULL)
				rip = page->u.irow;
			else
				++rip;
			if (idb->huffman_key != NULL ||
			    WT_ITEM_TYPE(item) == WT_ITEM_KEY_OVFL)
				WT_KEY_SET_PROCESS(rip, item);
			else
				WT_KEY_SET(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
			++indx_count;
			break;
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			/*
			 * If the second or subsequent duplicate, move to the
			 * next slot and copy the previous key.
			 */
			if (rip->data != NULL) {
				WT_KEY_SET(rip + 1, rip->key, rip->size);
				++rip;
				++indx_count;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			rip->data = item;
			++records;
			break;
		case WT_ITEM_OFF:
			rip->data = item;
			records += WT_ROW_OFF_RECORDS(rip);
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = indx_count;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static void
__wt_bt_page_inmem_col_int(WT_PAGE *page)
{
	WT_COL *cip;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i;

	hdr = page->hdr;
	cip = page->u.icol;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains WT_OFF structures.
	 */
	WT_OFF_FOREACH(page, off, i) {
		cip->data = off;
		++cip;
		records += WT_RECORDS(off);
	}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)off);
}

/*
 * __wt_bt_page_inmem_col_var --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static void
__wt_bt_page_inmem_col_var(WT_PAGE *page)
{
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;
	cip = page->u.icol;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains unsorted data items.  The data items are on-page
	 * data (WT_ITEM_DATA), overflow (WT_ITEM_DATA_OVFL) or deleted
	 * (WT_ITEM_DEL) items.
	 */
	WT_ITEM_FOREACH(page, item, i) {
		cip->data = item;
		++cip;
	}

	page->indx_count = page->records = hdr->u.entries;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)item);
}

/*
 * __wt_bt_page_inmem_dup_leaf --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	duplicate trees.
 */
static int
__wt_bt_page_inmem_dup_leaf(DB *db, WT_PAGE *page)
{
	WT_ROW *rip;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains sorted data items.  The data items are on-page
	 * (WT_ITEM_DATA_DUP) or overflow (WT_ITEM_DUP_OVFL) items.
	 *
	 * These data values are sorted, so we want to treat them as keys, and
	 * we return them as on-page WT_ITEM values, so we want to tream them
	 * as data.  Set both the WT_ROW key and data fields.
	 */
	rip = page->u.irow;
	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA_DUP:
			WT_KEY_SET(rip, WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
			break;
		case WT_ITEM_DATA_DUP_OVFL:
			WT_KEY_SET_PROCESS(rip, item);
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		rip->data = item;
		++rip;
	}

	page->indx_count = hdr->u.entries;
	page->records = hdr->u.entries;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_fix --
 *	Build in-memory index for fixed-length column-store leaf pages.
 */
static void
__wt_bt_page_inmem_col_fix(DB *db, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_HDR *hdr;
	u_int32_t i;
	u_int8_t *p;

	hdr = page->hdr;
	cip = page->u.icol;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains fixed-length objects.
	 */
	WT_FIX_FOREACH(db, page, p, i) {
		cip->data = p;
		++cip;
	}

	page->indx_count = page->records = hdr->u.entries;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)p);
}

/*
 * __wt_bt_page_inmem_col_rcc --
 *	Build in-memory index for repeat-compressed, fixed-length column-store
 *	leaf pages.
 */
static void
__wt_bt_page_inmem_col_rcc(DB *db, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i;
	u_int8_t *p;

	hdr = page->hdr;
	cip = page->u.icol;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains fixed-length objects.
	 */
	WT_RCC_REPEAT_FOREACH(db, page, p, i) {
		records += WT_RCC_REPEAT_COUNT(p);
		cip->data = p;
		++cip;
	}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_offset(page, (u_int8_t *)p);
}

/*
 * __wt_bt_item_process --
 *	Overflow and/or compressed on-page items need processing before
 *	we look at them.
 */
int
__wt_bt_item_process(
    WT_TOC *toc, WT_ITEM *item, WT_PAGE **ovfl_ret, DBT *dbt_ret)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_PAGE *ovfl;
	void *huffman, *p;
	u_int32_t size;
	int ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ovfl = NULL;
	ret = 0;

	/*
	 * 3 cases: compressed on-page item, or compressed or uncompressed
	 * overflow item.
	 *
	 * The return cases are confusing.   If the item is compressed (either
	 * on-page or overflow, it doesn't matter), then always uncompress it
	 * and return a copy in the return DBT, and ignore the return WT_PAGE.
	 * If the item isn't compressed, we're here because it's an overflow
	 * item.  If the caller passed in a return WT_PAGE, return the page to
	 * the caller and ignore the return DBT.  If the caller didn't pass in
	 * a return WT_PAGE, copy the page into the return DBT.
	 *
	 * The reason for this is that this function gets called in a couple
	 * of ways, and the additional complexity is worth it because avoiding
	 * a copy of an overflow chunk is a pretty good thing.
	 */
	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
		huffman = idb->huffman_key;
		goto onpage;
	case WT_ITEM_KEY_DUP:
	case WT_ITEM_DATA:
	case WT_ITEM_DATA_DUP:
		huffman = idb->huffman_data;
onpage:		p = WT_ITEM_BYTE(item);
		size = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_KEY_OVFL:
		huffman = idb->huffman_key;
		goto offpage;
	case WT_ITEM_KEY_DUP_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DATA_DUP_OVFL:
		huffman = idb->huffman_data;
offpage:	WT_RET(__wt_bt_ovfl_in(toc, WT_ITEM_BYTE_OVFL(item), &ovfl));
		p = WT_PAGE_BYTE(ovfl);
		size = ovfl->hdr->u.datalen;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * If the item is compressed (on-page or overflow), decode it and copy
	 * it into the caller's DBT.   If the item is not compressed, it's an
	 * overflow item (else we wouldn't be here) and return a pointer to the
	 * page itself, if the user passed in a WT_PAGE reference, otherwise
	 * copy it into the caller's DBT.
	 */
	if (huffman == NULL) {
		if (ovfl == NULL || ovfl_ret == NULL) {
			if (size > dbt_ret->mem_size)
				WT_ERR(__wt_realloc(env,
				    &dbt_ret->mem_size, size, &dbt_ret->data));
			memcpy(dbt_ret->data, p, size);
			dbt_ret->size = size;
		} else {
			*ovfl_ret = ovfl;
			ovfl = NULL;
		}
	} else
		WT_ERR(__wt_huffman_decode(huffman, p, size,
		    &dbt_ret->data, &dbt_ret->mem_size, &dbt_ret->size));

err:	if (ovfl != NULL)
		__wt_bt_page_out(toc, &ovfl, 0);

	return (ret);
}
