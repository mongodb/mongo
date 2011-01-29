/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_page_inmem_col_fix(DB *, WT_PAGE *);
static void __wt_page_inmem_col_int(WT_PAGE *);
static void __wt_page_inmem_col_rcc(DB *, WT_PAGE *);
static void __wt_page_inmem_col_var(WT_PAGE *);
static int  __wt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int  __wt_page_inmem_int_ref(WT_TOC *, uint32_t, WT_PAGE *);
static int  __wt_page_inmem_row_int(DB *, WT_PAGE *);
static int  __wt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in(
    WT_TOC *toc, WT_PAGE *parent, WT_REF *ref, WT_OFF *off, int dsk_verify)
{
	ENV *env;
	WT_CACHE *cache;
	int ret;

	env = toc->env;
	cache = env->ienv->cache;

	for (;;)
		switch (ref->state) {
		case WT_OK:
			/*
			 * The page is in memory: get a hazard reference, update
			 * the page's LRU and return.
			 */
			if (__wt_hazard_set(toc, ref)) {
				ref->page->read_gen = ++cache->read_gen;
				return (0);
			}
			/* FALLTHROUGH */
		case WT_EVICT:
			/*
			 * The page is being considered for eviction, wait for
			 * that to resolve.
			 */
			__wt_yield();
			break;
		case WT_EMPTY:
			/* The page isn't in memory, request it be read. */
			__wt_cache_read_serial(
			    toc, parent, ref, off, dsk_verify, ret);
			if (ret != 0)
				return (ret);
			break;
		default:
			WT_ABORT(env, "WT_REF->state invalid");
			break;
		}
	/* NOTREACHED */
}

/*
 * __wt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_page_inmem(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_PAGE_DISK *dsk;
	uint32_t nindx;
	int ret;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;
	ret = 0;

	WT_ASSERT(env, page->u.indx == NULL);

	/* Determine the maximum number of indexes we'll need for this page. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
		nindx = dsk->u.entries;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		nindx = dsk->u.entries / 2;
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Row store leaf pages support duplicates, so the real worst
		 * case is one key plus some number of duplicate data items.
		 * The number is configurable, that is, you can configure when
		 * a duplicate set is big enough to be pushed off the page;
		 * we're conservative here.
		 */
		nindx = dsk->u.entries - 1;
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
	switch (dsk->type) {
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
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_page_inmem_int_ref(toc, nindx, page));
		break;
	default:
		break;
	}

	/* Fill in the structures. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		__wt_page_inmem_col_fix(db, page);
		break;
	case WT_PAGE_COL_INT:
		__wt_page_inmem_col_int(page);
		break;
	case WT_PAGE_COL_RCC:
		__wt_page_inmem_col_rcc(db, page);
		break;
	case WT_PAGE_COL_VAR:
		__wt_page_inmem_col_var(page);
		break;
	case WT_PAGE_DUP_LEAF:
		WT_ERR(__wt_page_inmem_dup_leaf(db, page));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_page_inmem_row_int(db, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_page_inmem_row_leaf(db, page));
		break;
	default:
		break;
	}
	return (0);

err:	__wt_page_discard(toc, page);
	return (ret);
}

/*
 * __wt_page_inmem_col_fix --
 *	Build in-memory index for fixed-length column-store leaf pages.
 */
static void
__wt_page_inmem_col_fix(DB *db, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint32_t i;
	uint8_t *p;

	dsk = page->dsk;
	cip = page->u.icol;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains fixed-length objects.
	 */
	WT_FIX_FOREACH(db, page, p, i) {
		cip->data = p;
		++cip;
	}

	page->indx_count = page->records = dsk->u.entries;
}

/*
 * __wt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static void
__wt_page_inmem_col_int(WT_PAGE *page)
{
	WT_COL *cip;
	WT_OFF *off;
	WT_PAGE_DISK *dsk;
	uint64_t records;
	uint32_t i;

	dsk = page->dsk;
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

	page->indx_count = dsk->u.entries;
	page->records = records;
}

/*
 * __wt_page_inmem_col_rcc --
 *	Build in-memory index for repeat-compressed, fixed-length column-store
 *	leaf pages.
 */
static void
__wt_page_inmem_col_rcc(DB *db, WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint64_t records;
	uint32_t i;
	uint8_t *p;

	dsk = page->dsk;
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

	page->indx_count = dsk->u.entries;
	page->records = records;
}

/*
 * __wt_page_inmem_col_var --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static void
__wt_page_inmem_col_var(WT_PAGE *page)
{
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;
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

	page->indx_count = page->records = dsk->u.entries;
}

/*
 * __wt_page_inmem_dup_leaf --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	duplicate trees.
 */
static int
__wt_page_inmem_dup_leaf(DB *db, WT_PAGE *page)
{
	WT_ROW *rip;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;

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
			__wt_key_set
			    (rip, WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
			break;
		case WT_ITEM_DATA_DUP_OVFL:
			__wt_key_set_process(rip, item);
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		rip->data = item;
		++rip;
	}

	page->indx_count = dsk->u.entries;
	page->records = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_row_int --
 *	Build in-memory index for row-store and off-page duplicate tree
 *	internal pages.
 */
static int
__wt_page_inmem_row_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint64_t records;
	uint32_t i;
	void *huffman;

	idb = db->idb;
	dsk = page->dsk;
	rip = page->u.irow;
	records = 0;

	huffman =
	    dsk->type == WT_PAGE_DUP_INT ? idb->huffman_data : idb->huffman_key;

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
				__wt_key_set(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
			__wt_key_set_process(rip, item);
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			records += WT_RECORDS(off);
			rip->data = item;
			++rip;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = dsk->u.entries / 2;
	page->records = records;
	return (0);
}

/*
 * __wt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
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
				__wt_key_set_process(rip, item);
			else
				__wt_key_set(rip,
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
				__wt_key_set(rip + 1, rip->key, rip->size);
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
 * __wt_item_process --
 *	Overflow and/or compressed on-page items need processing before
 *	we look at them.
 */
int
__wt_item_process(WT_TOC *toc, WT_ITEM *item, DBT *dbt_ret)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	IDB *idb;
	uint32_t size;
	int ret;
	void *huffman, *p;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	idb = db->idb;
	ret = 0;

	/*
	 * 3 cases: compressed on-page item, or compressed or uncompressed
	 * overflow item.
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
offpage:	/*
		 * It's an overflow item -- if it's not encoded, we can read
		 * it directly into the user's return DBT, otherwise we have to
		 * have our own buffer as temporary space, and the decode call
		 * will put a decoded version into the user's return DBT.
		 */
		if (huffman == NULL)
			tmp = dbt_ret;
		else
			WT_RET(__wt_scr_alloc(toc, 0, &tmp));
		WT_RET(__wt_ovfl_in(toc, WT_ITEM_BYTE_OVFL(item), tmp));
		p = tmp->data;
		size = tmp->size;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * If the item is not compressed, and it's not an overflow item, copy
	 * it into the caller's DBT.  If the item is not compressed, and it's
	 * an overflow item, it was already copied into the caller's DBT.
	 *
	 * If the item is compressed, pass it to the decode routines, they'll
	 * copy a decoded version into the caller's DBT.
	 */
	if (huffman == NULL) {
		if (tmp != dbt_ret) {
			 if (size > dbt_ret->mem_size)
				 WT_ERR(__wt_realloc(
				     env, &dbt_ret->mem_size,
				     size, &dbt_ret->data));
			memcpy(dbt_ret->data, p, size);
			dbt_ret->size = size;
		}
	} else
		WT_ERR(__wt_huffman_decode(huffman, p, size,
		    &dbt_ret->data, &dbt_ret->mem_size, &dbt_ret->size));

err:	if (tmp != NULL && tmp != dbt_ret)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_page_inmem_int_ref --
 *	Allocate and initialize the reference array for internal pages.
 */
static int
__wt_page_inmem_int_ref(WT_TOC *toc, uint32_t nindx, WT_PAGE *page)
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

/*
 * __wt_key_set --
 *	Set a key/size pair, where the key does not require further processing.
 */
inline void
__wt_key_set(WT_ROW *rip, void *key, uint32_t size)
{
	rip->key = key;
	rip->size = size;
}

/*
 * __wt_key_set_process --
 *	Set a key/size pair, where the key requires further processing.
 */
inline void
__wt_key_set_process(WT_ROW *rip, void *key)
{
	rip->key = key;
	rip->size = 0;
}

/*
 * __wt_key_process --
 *	Return if a key requires processing.
 */
inline int
__wt_key_process(WT_ROW *rip)
{
	return (rip->size == 0 ? 1 : 0);
}
