/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_page_inmem_col_fix(WT_TOC *, WT_PAGE *);
static int __wt_page_inmem_col_int(WT_TOC *, WT_PAGE *);
static int __wt_page_inmem_col_rle(WT_TOC *, WT_PAGE *);
static int __wt_page_inmem_col_var(WT_TOC *, WT_PAGE *);
static int __wt_page_inmem_row_int(WT_TOC *, WT_PAGE *);
static int __wt_page_inmem_row_leaf(WT_TOC *, WT_PAGE *);

/*
 * __wt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in(WT_TOC *toc, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
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
			 * the page's LRU and return.  The expected reason we
			 * can't get a hazard reference is because the page is
			 * being evicted; yield and try again.
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
		case WT_REF_DELETED:
			/*
			 * if the page were to be emptied and reconciled, it may
			 * have been deleted.  Any thread walking the tree needs
			 * to be notified.
			 */
			return (WT_PAGE_DELETED);
		case WT_REF_DISK:
			/* The page isn't in memory, request it be read. */
			__wt_cache_read_serial(
			    toc, parent, ref, dsk_verify, ret);
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
	WT_PAGE_DISK *dsk;
	int ret;

	db = toc->db;
	dsk = page->dsk;
	ret = 0;

	WT_ASSERT(toc->env, dsk->u.entries > 0);
	WT_ASSERT(toc->env, page->indx_count == 0);

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_page_inmem_col_fix(toc, page));
		break;
	case WT_PAGE_COL_INT:
		WT_ERR(__wt_page_inmem_col_int(toc, page));
		break;
	case WT_PAGE_COL_RLE:
		WT_ERR(__wt_page_inmem_col_rle(toc, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_ERR(__wt_page_inmem_col_var(toc, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_page_inmem_row_int(toc, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_page_inmem_row_leaf(toc, page));
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (0);

err:	__wt_page_discard(toc, page);
	return (ret);
}

/*
 * __wt_page_inmem_col_fix --
 *	Build in-memory index for fixed-length column-store leaf pages.
 */
static int
__wt_page_inmem_col_fix(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint32_t i;
	uint8_t *p;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET((
	    __wt_calloc_def(env, (size_t)dsk->u.entries, &page->u.col_leaf.d)));

	/*
	 * Walk the page, building references: the page contains fixed-length
	 * objects.
	 */
	cip = page->u.col_leaf.d;
	WT_FIX_FOREACH(db, dsk, p, i)
		(cip++)->data = WT_PAGE_DISK_OFFSET(dsk, p);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static int
__wt_page_inmem_col_int(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_COL_REF *cref;
	WT_OFF_RECORD *off_record;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	env = toc->env;
	dsk = page->dsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a offset object).
	 */
	WT_RET((
	    __wt_calloc_def(env, (size_t)dsk->u.entries, &page->u.col_int.t)));

	/*
	 * Walk the page, building references: the page contains WT_OFF_RECORD
	 * structures.
	 */
	cref = page->u.col_int.t;
	WT_OFF_FOREACH(dsk, off_record, i) {
		WT_COL_REF_ADDR(cref) = off_record->addr;
		WT_COL_REF_SIZE(cref) = off_record->size;
		cref->recno = WT_RECNO(off_record);
		++cref;
	}

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_col_rle --
 *	Build in-memory index for fixed-length, run-length encoded, column-store
 *	leaf pages.
 */
static int
__wt_page_inmem_col_rle(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint32_t i;
	uint8_t *p;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET((
	    __wt_calloc_def(env, (size_t)dsk->u.entries, &page->u.col_leaf.d)));

	/*
	 * Walk the page, building references: the page contains fixed-length
	 * objects.
	 */
	cip = page->u.col_leaf.d;
	WT_RLE_REPEAT_FOREACH(db, dsk, p, i)
		(cip++)->data = WT_PAGE_DISK_OFFSET(dsk, p);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_col_var --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static int
__wt_page_inmem_col_var(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	env = toc->env;
	dsk = page->dsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET((
	    __wt_calloc_def(env, (size_t)dsk->u.entries, &page->u.col_leaf.d)));

	/*
	 * Walk the page, building references: the page contains unsorted data
	 * items.  The data items are on-page data (WT_ITEM_DATA), overflow
	 * (WT_ITEM_DATA_OVFL) or deleted (WT_ITEM_DEL) items.
	 */
	cip = page->u.col_leaf.d;
	WT_ITEM_FOREACH(dsk, item, i)
		(cip++)->data = WT_PAGE_DISK_OFFSET(dsk, item);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_row_int --
 *	Build in-memory index for row-store internal pages.
 */
static int
__wt_page_inmem_row_int(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	BTREE *btree;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE_DISK *dsk;
	WT_ROW_REF *rref;
	uint32_t i, nindx;
	void *huffman;

	env = toc->env;
	btree = toc->db->btree;
	dsk = page->dsk;
	env = toc->env;
	huffman = btree->huffman_key;

	/*
	 * Internal row-store page entries map one-to-two to the number of
	 * physical entries on the page (each physical entry is a data item
	 * and offset object).
	 */
	nindx = dsk->u.entries / 2;
	WT_RET((__wt_calloc_def(env, (size_t)nindx, &page->u.row_int.t)));

	/*
	 * Walk the page, building references: the page contains sorted key and
	 * offpage-reference pairs.  Keys are row store internal pages with
	 * on-page/overflow (WT_ITEM_KEY/KEY_OVFL) items, and offpage references
	 * are WT_ITEM_OFF items.
	 */
	rref = page->u.row_int.t;
	WT_ITEM_FOREACH(dsk, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			if (huffman == NULL) {
				__wt_key_set(rref,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			__wt_key_set_process(rref, item);
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			WT_COL_REF_ADDR(rref) = off->addr;
			WT_COL_REF_SIZE(rref) = off->size;
			++rref;
			break;
		}

	page->indx_count = nindx;
	return (0);
}

/*
 * __wt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_page_inmem_row_leaf(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	BTREE *btree;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i, nindx;

	env = toc->env;
	btree = toc->db->btree;
	dsk = page->dsk;

	/*
	 * Leaf row-store page entries map to a maximum of two-to-one to the
	 * number of physical entries on the page (each physical entry might
	 * be a key without any subsequent data item).
	 */
	WT_RET((__wt_calloc_def(
	    env, (size_t)dsk->u.entries * 2, &page->u.row_leaf.d)));

	/*
	 * Walk a row-store page of WT_ITEMs, building indices and finding the
	 * end of the page.
	 *
	 * The page contains key/data pairs.  Keys are on-page (WT_ITEM_KEY) or
	 * overflow (WT_ITEM_KEY_OVFL) items, data are either a single on-page
	 * (WT_ITEM_DATA) or overflow (WT_ITEM_DATA_OVFL) item.
	 */
	nindx = 0;
	rip = page->u.row_leaf.d;
	WT_ITEM_FOREACH(dsk, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			++nindx;
			if (rip->key != NULL)
				++rip;
			if (btree->huffman_key != NULL ||
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
			rip->data = &btree->empty_item;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			rip->data = item;
			break;
		}

	page->indx_count = nindx;
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
	BTREE *btree;
	uint32_t size;
	int ret;
	void *huffman, *p;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	btree = db->btree;
	ret = 0;

	/*
	 * 3 cases: compressed on-page item or, compressed or uncompressed
	 * overflow item.
	 */
	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
		huffman = btree->huffman_key;
		goto onpage;
	case WT_ITEM_KEY_OVFL:
		huffman = btree->huffman_key;
		goto offpage;
	case WT_ITEM_DATA:
		huffman = btree->huffman_data;
onpage:		p = WT_ITEM_BYTE(item);
		size = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_DATA_OVFL:
		huffman = btree->huffman_data;
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
