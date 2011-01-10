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
static int  __wt_bt_page_inmem_int_ref(WT_TOC *, uint32_t, WT_PAGE *);
static int  __wt_bt_page_inmem_row_int(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_bt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_bt_page_in(WT_TOC *toc, WT_REF *ref, WT_OFF *off, int dsk_verify)
{
	WT_CACHE *cache;
	int ret;

	cache = toc->env->ienv->cache;

	for (;;)
		switch (ref->state) {
		case WT_OK:
			/* The page is in memory, update it's LRU and return. */
			if (__wt_hazard_set(toc, ref)) {
				ref->page->read_gen = ++cache->read_gen;
				return (0);
			}
			/* FALLTHROUGH */
		case WT_DRAIN:
			/*
			 * The page is being considered for eviction, wait for
			 * that to resolve.
			 */
			__wt_yield();
			break;
		case WT_EMPTY:
			/* The page isn't in memory, request it be read. */
			__wt_cache_read_serial(toc, ref, off, dsk_verify, ret);
			if (ret != 0)
				return (ret);
			break;
		default:
			break;
		}
	/* NOTREACHED */
}

/*
 * __wt_bt_page_out --
 *	Release a hazard reference to a page.
 */
void
__wt_bt_page_out(WT_TOC *toc, WT_PAGE **pagep, uint32_t flags)
{
	ENV *env;
	WT_PAGE *page;

	env = toc->env;

	WT_ENV_FCHK_ASSERT(
	    env, "__wt_bt_page_out", flags, WT_APIMASK_BT_PAGE_OUT);

	/*
	 * Clear the caller's reference so we don't accidentally use a page
	 * after discarding our reference, and to make it easy to decide if
	 * a page is in-use after our return.
	 */
	page = *pagep;
	*pagep = NULL;

	/* Discard the hazard reference. */
	__wt_hazard_clear(toc, page);

	/*
	 * If it's an overflow page, it was never hooked into the tree, free
	 * the memory.   In some rare cases other pages are standalone, and
	 * they're discarded as well.
	 */
	if (page->hdr->type == WT_PAGE_OVFL || LF_ISSET(WT_DISCARD))
		__wt_bt_page_discard(toc, page);
}

/*
 * __wt_bt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_bt_page_inmem(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_PAGE_HDR *hdr;
	uint32_t nindx;
	int ret;

	db = toc->db;
	env = toc->env;
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
	case WT_PAGE_OVFL:
		return (0);
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
		WT_ERR((__wt_calloc(env,
		    nindx, sizeof(WT_COL), &page->u.icol)));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_ERR((__wt_calloc(env,
		    nindx, sizeof(WT_ROW), &page->u.irow)));
		break;
	default:
		break;
	}

	/* Allocate reference array for internal pages. */
	switch (hdr->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_bt_page_inmem_int_ref(toc, nindx, page));
		break;
	default:
		break;
	}

	/* Fill in the structures. */
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
	case WT_PAGE_DUP_LEAF:
		WT_ERR(__wt_bt_page_inmem_dup_leaf(db, page));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_bt_page_inmem_row_int(db, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_bt_page_inmem_row_leaf(db, page));
		break;
	default:
		break;
	}
	return (0);

err:	__wt_bt_page_discard(toc, page);
	return (ret);
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
	uint32_t i;
	uint8_t *p;

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
	uint64_t records;
	uint32_t i;

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
	uint64_t records;
	uint32_t i;
	uint8_t *p;

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
	uint32_t i;

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
	uint32_t i;

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
	return (0);
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
	uint64_t records;
	uint32_t i;
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
	return (0);
}

/*
 * __wt_bt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_bt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
{
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_REF *ref;
	WT_ROW *rip;
	uint32_t i, indx_count;
	uint64_t records;

	env = db->env;
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

			/*
			 * We need a WT_REF entry for any item referencing an
			 * off-page duplicate tree.  Create the array of WT_REF
			 * pointers and fill in a WT_REF structure.
			 */
			if (page->u3.dup == NULL)
				WT_RET(__wt_calloc(env, indx_count,
				    sizeof(WT_REF *), &page->u3.dup));
			WT_RET(__wt_calloc(env, 1, sizeof(WT_REF), &ref));
			ref->state = WT_EMPTY;
			page->u3.dup[WT_ROW_SLOT(page, rip)] = ref;

			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = indx_count;
	page->records = records;

	return (0);
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
	uint32_t size;
	int ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ovfl = NULL;
	if (ovfl_ret != NULL)
		*ovfl_ret = NULL;
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
offpage:	WT_RET(__wt_bt_ovfl_in(toc, WT_ITEM_BYTE_OVFL(item), &ovfl, 0));
		p = WT_PAGE_BYTE(ovfl);
		size = ovfl->hdr->u.datalen;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * If the item is NOT compressed it's an overflow item (otherwise we
	 * wouldn't be in this code); return a pointer to the page itself, if
	 * the user passed in a WT_PAGE reference, otherwise copy it into the
	 * caller's DBT.
	 *
	 * If the item is compressed (on-page or overflow), decode it and copy
	 * it into the caller's DBT, never returning a copy of the page.
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

/*
 * __wt_bt_page_inmem_int_ref --
 *	Allocate and initialize the reference array for internal pages.
 */
static int
__wt_bt_page_inmem_int_ref(WT_TOC *toc, uint32_t nindx, WT_PAGE *page)
{
	ENV *env;
	WT_REF *cp;
	uint32_t i;

	env = toc->env;

	/*
	 * Allocate an array of WT_REF structures for internal pages.  In the
	 * case of an internal page, we know all of the slots are going to be
	 * filled in -- every slot on the page references a subtree.  In the
	 * case of row-store leaf pages, the only slots that get filled in are
	 * slots that reference off-page duplicate trees.   So, if it's an
	 * internal page, it's a simple one-time allocation; if a leaf page,
	 * we'll do similar work, but lazily in the routine that fills in the
	 * in-memory information.
	 */
	WT_RET(__wt_calloc(
	    env, nindx, sizeof(WT_REF), &page->u3.ref));
	for (i = 0, cp = page->u3.ref; i < nindx; ++i, ++cp)
		cp->state = WT_EMPTY;
	return (0);
}
