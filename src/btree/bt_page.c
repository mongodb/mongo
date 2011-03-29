/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int __wt_page_inmem_col_fix(SESSION *, WT_PAGE *);
static int __wt_page_inmem_col_int(SESSION *, WT_PAGE *);
static int __wt_page_inmem_col_rle(SESSION *, WT_PAGE *);
static int __wt_page_inmem_col_var(SESSION *, WT_PAGE *);
static int __wt_page_inmem_row_int(SESSION *, WT_PAGE *);
static int __wt_page_inmem_row_leaf(SESSION *, WT_PAGE *);

/*
 * __wt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in(SESSION *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
{
	WT_CACHE *cache;
	int ret;

	cache = S2C(session)->cache;

	for (;;)
		switch (WT_REF_STATE(ref->state)) {
		case WT_REF_MEM:
			/*
			 * The page is in memory: get a hazard reference, update
			 * the page's LRU and return.  The expected reason we
			 * can't get a hazard reference is because the page is
			 * being evicted; yield and try again.
			 */
			if (__wt_hazard_set(session, ref)) {
				ref->page->read_gen = ++cache->read_gen;
				return (0);
			}
			__wt_yield();
			break;
		case WT_REF_DISK:
		case WT_REF_EVICTED:
			/* The page isn't in memory, request it be read. */
			__wt_cache_read_serial(
			    session, parent, ref, dsk_verify, ret);
			if (ret != 0)
				return (ret);
			break;
		default:
			WT_ABORT(session, "WT_REF->state invalid");
			break;
		}
	/* NOTREACHED */
}

/*
 * __wt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_page_inmem(SESSION *session, WT_PAGE *page)
{
	WT_PAGE_DISK *dsk;
	int ret;

	dsk = page->XXdsk;
	ret = 0;

	WT_ASSERT(session, dsk->u.entries > 0);
	WT_ASSERT(session, page->indx_count == 0);

	page->type = dsk->type;

	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		page->u.col_int.recno = dsk->recno;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		page->u.col_leaf.recno = dsk->recno;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_page_inmem_col_fix(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_ERR(__wt_page_inmem_col_int(session, page));

		/* Column-store internal pages do not require a disk image. */
		__wt_free(session, page->XXdsk);
		break;
	case WT_PAGE_COL_RLE:
		WT_ERR(__wt_page_inmem_col_rle(session, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_ERR(__wt_page_inmem_col_var(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_page_inmem_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_page_inmem_row_leaf(session, page));
		break;
	}
	return (0);

err:	__wt_page_discard(session, page);
	return (ret);
}

/*
 * __wt_page_inmem_col_fix --
 *	Build in-memory index for fixed-length column-store leaf pages.
 */
static int
__wt_page_inmem_col_fix(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint32_t i;
	uint8_t *p;

	btree = session->btree;
	dsk = page->XXdsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET(__wt_calloc_def(
	    session, (size_t)dsk->u.entries, &page->u.col_leaf.d));

	/*
	 * Walk the page, building references: the page contains fixed-length
	 * objects.
	 */
	cip = page->u.col_leaf.d;
	WT_FIX_FOREACH(btree, dsk, p, i)
		(cip++)->value = WT_PAGE_DISK_OFFSET(dsk, p);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static int
__wt_page_inmem_col_int(SESSION *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD *off_record;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->XXdsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a offset object).
	 */
	WT_RET(__wt_calloc_def(
	    session, (size_t)dsk->u.entries, &page->u.col_int.t));

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
__wt_page_inmem_col_rle(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	uint32_t i;
	uint8_t *p;

	btree = session->btree;
	dsk = page->XXdsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET(__wt_calloc_def(
	    session, (size_t)dsk->u.entries, &page->u.col_leaf.d));

	/*
	 * Walk the page, building references: the page contains fixed-length
	 * objects.
	 */
	cip = page->u.col_leaf.d;
	WT_RLE_REPEAT_FOREACH(btree, dsk, p, i)
		(cip++)->value = WT_PAGE_DISK_OFFSET(dsk, p);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_col_var --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static int
__wt_page_inmem_col_var(SESSION *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->XXdsk;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET(__wt_calloc_def(
	    session, (size_t)dsk->u.entries, &page->u.col_leaf.d));

	/*
	 * Walk the page, building references: the page contains unsorted data
	 * items.  The data items are on-page data (WT_CELL_DATA), overflow
	 * (WT_CELL_DATA_OVFL) or deleted (WT_CELL_DEL) items.
	 */
	cip = page->u.col_leaf.d;
	WT_CELL_FOREACH(dsk, cell, i)
		(cip++)->value = WT_PAGE_DISK_OFFSET(dsk, cell);

	page->indx_count = dsk->u.entries;
	return (0);
}

/*
 * __wt_page_inmem_row_int --
 *	Build in-memory index for row-store internal pages.
 */
static int
__wt_page_inmem_row_int(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_CELL *cell;
	WT_OFF *off;
	WT_PAGE_DISK *dsk;
	WT_ROW_REF *rref;
	uint32_t i, nindx;
	void *huffman;

	btree = session->btree;
	dsk = page->XXdsk;
	huffman = btree->huffman_key;

	/*
	 * Internal row-store page entries map one-to-two to the number of
	 * physical entries on the page (each physical entry is a data item
	 * and offset object).
	 */
	nindx = dsk->u.entries / 2;
	WT_RET((__wt_calloc_def(session, (size_t)nindx, &page->u.row_int.t)));

	/*
	 * Walk the page, building references: the page contains sorted key and
	 * offpage-reference pairs.  Keys are row store internal pages with
	 * on-page/overflow (WT_CELL_KEY/KEY_OVFL) items, and offpage references
	 * are WT_CELL_OFF items.
	 */
	rref = page->u.row_int.t;
	WT_CELL_FOREACH(dsk, cell, i)
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_KEY:
			if (huffman == NULL) {
				__wt_key_set(rref,
				    WT_CELL_BYTE(cell), WT_CELL_LEN(cell));
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_KEY_OVFL:
			__wt_key_set_process(rref, cell);
			break;
		case WT_CELL_OFF:
			off = WT_CELL_BYTE_OFF(cell);
			WT_ROW_REF_ADDR(rref) = off->addr;
			WT_ROW_REF_SIZE(rref) = off->size;
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
__wt_page_inmem_row_leaf(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i, nindx;

	btree = session->btree;
	dsk = page->XXdsk;

	/*
	 * Leaf row-store page entries map to a maximum of two-to-one to the
	 * number of physical entries on the page (each physical entry might
	 * be a key without any subsequent data item).
	 */
	WT_RET((__wt_calloc_def(
	    session, (size_t)dsk->u.entries * 2, &page->u.row_leaf.d)));

	/*
	 * Walk a row-store page of WT_CELLs, building indices and finding the
	 * end of the page.
	 *
	 * The page contains key/data pairs.  Keys are on-page (WT_CELL_KEY) or
	 * overflow (WT_CELL_KEY_OVFL) items, data are either a single on-page
	 * (WT_CELL_DATA) or overflow (WT_CELL_DATA_OVFL) item.
	 */
	nindx = 0;
	rip = page->u.row_leaf.d;
	WT_CELL_FOREACH(dsk, cell, i)
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			++nindx;
			if (rip->key != NULL)
				++rip;
			if (btree->huffman_key != NULL ||
			    WT_CELL_TYPE(cell) == WT_CELL_KEY_OVFL)
				__wt_key_set_process(rip, cell);
			else
				__wt_key_set(rip,
				    WT_CELL_BYTE(cell), WT_CELL_LEN(cell));

			/*
			 * Two keys in a row, or a key at the end of the page
			 * implies a zero-length data item.  Initialize the
			 * slot as if it's going to happen.
			 */
			rip->value = WT_ROW_EMPTY;
			break;
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
			rip->value = WT_PAGE_DISK_OFFSET(dsk, cell);
			break;
		}

	page->indx_count = nindx;
	return (0);
}

/*
 * __wt_cell_process --
 *	Copy an on-page key into a return buffer, processing as needed.
 */
int
__wt_cell_process(SESSION *session, WT_CELL *cell, WT_BUF *retbuf)
{
	BTREE *btree;
	WT_BUF *tmp;
	uint32_t size;
	int ret;
	void *huffman;
	const void *p;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	switch (WT_CELL_TYPE(cell)) {
	case WT_CELL_KEY:
		huffman = btree->huffman_key;
		goto onpage;
	case WT_CELL_KEY_OVFL:
		huffman = btree->huffman_key;
		goto offpage;
	case WT_CELL_DATA:
		huffman = btree->huffman_data;
onpage:		p = WT_CELL_BYTE(cell);
		size = WT_CELL_LEN(cell);
		break;
	case WT_CELL_DATA_OVFL:
		huffman = btree->huffman_data;
offpage:	/*
		 * It's an overflow item -- if it's not encoded, we can read it
		 * directly into the user's return WT_ITEM, otherwise we have
		 * to have our own buffer as temporary space, and the decode
		 * call will put a decoded version into the user's return
		 * WT_ITEM.
		 */
		if (huffman == NULL)
			tmp = retbuf;
		else
			WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_RET(__wt_ovfl_in(session, WT_CELL_BYTE_OVFL(cell), tmp));
		p = tmp->item.data;
		size = tmp->item.size;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * If the item is not compressed, and it is not an overflow item, copy
	 * it into the caller's WT_BUF.  If the item is not compressed, and
	 * it is an overflow item, it was already copied into the caller's
	 * WT_BUF.
	 *
	 * If the item is compressed, pass it to the decode routines, they'll
	 * copy a decoded version into the caller's WT_BUF.
	 */
	if (huffman == NULL) {
		if (tmp != retbuf) {
			WT_ERR(__wt_buf_grow(session, retbuf, size));
			memcpy(retbuf->mem, p, size);
		}
	} else
		WT_ERR(__wt_huffman_decode(huffman, p, size, retbuf));

err:	if (tmp != NULL && tmp != retbuf)
		__wt_scr_release(&tmp);

	return (ret);
}
