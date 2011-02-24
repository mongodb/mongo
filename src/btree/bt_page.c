/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static void __wt_page_inmem_col_fix(DB *, WT_PAGE *);
static void __wt_page_inmem_col_int(WT_PAGE *);
static void __wt_page_inmem_col_rle(DB *, WT_PAGE *);
static void __wt_page_inmem_col_var(WT_PAGE *);
static int  __wt_page_inmem_row_int(DB *, WT_PAGE *);
static int  __wt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in(
    WT_TOC *toc, WT_PAGE *parent, WT_REF *ref, void *off, int dsk_verify)
{
	ENV *env;
	WT_CACHE *cache;
	int ret;

	env = toc->env;
	cache = env->ienv->cache;

	for (;;)
		switch (ref->state) {
		case WT_REF_CACHE:
			/*
			 * The page is in memory: get a hazard reference, update
			 * the page's LRU and return.
			 */
			if (__wt_hazard_set(toc, ref)) {
				ref->page->read_gen = ++cache->read_gen;
				return (0);
			}
			/* FALLTHROUGH */
		case WT_REF_EVICT:
			/*
			 * The page is being considered for eviction, wait for
			 * that to resolve.
			 */
			__wt_yield();
			break;
		case WT_REF_DISK:
			/*
			 * The page isn't in memory, request it be read.  There
			 * is one additional special case: if the page were to
			 * be emptied and reconciled, it may have been deleted,
			 * in which case we return that fact.
			 *
			 * Note, the underlying read server does not check for
			 * this case: it's not necessary, because the address
			 * was set to WT_ADDR_DELETED before the WT_REF entry
			 * was reset to WT_REF_DISK.
			 */
			if (((WT_OFF *)off)->addr == WT_ADDR_DELETED)
				return (WT_PAGE_DELETED);
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
	WT_REF *cp;
	uint32_t i, nindx;
	int ret;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;
	ret = 0;

	WT_ASSERT(env, dsk->u.entries > 0);
	WT_ASSERT(env, page->u.indx == NULL);

	/*
	 * Determine the maximum number of indexes we'll need for this page
	 * and allocate an array of WT_{ROW,COL}_INDX structures.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		/*
		 * Column-store page entries map one-to-one to the number of
		 * physical entries on the page (each physical entry is a
		 * data item).
		 */
		nindx = dsk->u.entries;
		WT_ERR((
		    __wt_calloc(env, nindx, sizeof(WT_COL), &page->u.icol)));
		break;
	case WT_PAGE_ROW_INT:
		/*
		 * Internal row-store page entries map one-to-two to the number
		 * of physical entries on the page (each physical entry is a
		 * data item/offset pair).
		 */
		nindx = dsk->u.entries / 2;
		WT_ERR((
		    __wt_calloc(env, nindx, sizeof(WT_ROW), &page->u.irow)));
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Leaf row-store page entries map two-to-one to the number of
		 * physical entries on the page (each physical entry might be
		 * a key without any subsequent data item).
		 */
		nindx = dsk->u.entries * 2;
		WT_ERR((
		    __wt_calloc(env, nindx, sizeof(WT_ROW), &page->u.irow)));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Allocate an array of WT_REF structures for internal pages. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_calloc(env, nindx, sizeof(WT_REF), &page->u2.ref));
		for (i = 0, cp = page->u2.ref; i < nindx; ++i, ++cp)
			cp->state = WT_REF_DISK;
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
	case WT_PAGE_COL_RLE:
		__wt_page_inmem_col_rle(db, page);
		break;
	case WT_PAGE_COL_VAR:
		__wt_page_inmem_col_var(page);
		break;
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_page_inmem_row_int(db, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_page_inmem_row_leaf(db, page));
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
	WT_FIX_FOREACH(db, dsk, p, i)
		(cip++)->data = p;

	page->indx_count = dsk->u.entries;
}

/*
 * __wt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static void
__wt_page_inmem_col_int(WT_PAGE *page)
{
	WT_COL *cip;
	WT_OFF_RECORD *off_record;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;
	cip = page->u.icol;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains WT_OFF_RECORD structures.
	 */
	WT_OFF_FOREACH(dsk, off_record, i)
		(cip++)->data = off_record;

	page->indx_count = dsk->u.entries;
}

/*
 * __wt_page_inmem_col_rle --
 *	Build in-memory index for fixed-length, run-length encoded, column-store
 *	leaf pages.
 */
static void
__wt_page_inmem_col_rle(DB *db, WT_PAGE *page)
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
	WT_RLE_REPEAT_FOREACH(db, dsk, p, i)
		(cip++)->data = p;

	page->indx_count = dsk->u.entries;
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
	WT_ITEM_FOREACH(dsk, item, i)
		(cip++)->data = item;

	page->indx_count = dsk->u.entries;
}

/*
 * __wt_page_inmem_row_int --
 *	Build in-memory index for row-store internal pages.
 */
static int
__wt_page_inmem_row_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i;
	void *huffman;

	idb = db->idb;
	dsk = page->dsk;
	rip = page->u.irow;
	huffman = idb->huffman_key;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 * The page contains sorted key/offpage-reference pairs.  Keys are row
	 * store internal pages with on-page/overflow (WT_ITEM_KEY/KEY_OVFL)
	 * items, and offpage references are WT_ITEM_OFF items.
	 */
	WT_ITEM_FOREACH(dsk, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			if (huffman == NULL) {
				__wt_key_set(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			__wt_key_set_process(rip, item);
			break;
		case WT_ITEM_OFF:
			(rip++)->data = item;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = dsk->u.entries / 2;
	return (0);
}

/*
 * __wt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i;

	idb = db->idb;
	dsk = page->dsk;

	/*
	 * Walk a row-store page of WT_ITEMs, building indices and finding the
	 * end of the page.
	 *
	 * The page contains key/data pairs.  Keys are on-page (WT_ITEM_KEY) or
	 * overflow (WT_ITEM_KEY_OVFL) items, data are either a single on-page
	 * (WT_ITEM_DATA) or overflow (WT_ITEM_DATA_OVFL) item.
	 */
	rip = page->u.irow;
	WT_ITEM_FOREACH(dsk, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (rip->key != NULL)
				++rip;
			if (idb->huffman_key != NULL ||
			    WT_ITEM_TYPE(item) == WT_ITEM_KEY_OVFL)
				__wt_key_set_process(rip, item);
			else
				__wt_key_set(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));

			/*
			 * Two keys in a row, or a key at the end of the page
			 * implies a zero-length data item.  Initialize the
			 * slot as if it's going to happen.
			 */
			rip->data = &idb->empty_item;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			rip->data = item;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	/*
	 * Calculate the number of entries we actually found: rip references
	 * the slot last filled in, so increment it and do the calculation.
	 */
	++rip;
	page->indx_count = WT_ROW_SLOT(page, rip);

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
	 * 3 cases: compressed on-page item or, compressed or uncompressed
	 * overflow item.
	 */
	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
		huffman = idb->huffman_key;
		goto onpage;
	case WT_ITEM_KEY_OVFL:
		huffman = idb->huffman_key;
		goto offpage;
	case WT_ITEM_DATA:
		huffman = idb->huffman_data;
onpage:		p = WT_ITEM_BYTE(item);
		size = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_DATA_OVFL:
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
				 WT_ERR(__wt_realloc(env,
				     &dbt_ret->mem_size, size, &dbt_ret->data));
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
