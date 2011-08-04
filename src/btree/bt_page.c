/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __inmem_col_fix(WT_PAGE *);
static int  __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_row_leaf_keys(WT_SESSION_IMPL *, WT_PAGE *);
static void __inmem_row_leaf_slots(uint8_t *, uint32_t, uint32_t, uint32_t);

/*
 * __wt_page_in --
 *	Acquire a hazard reference to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in_func(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	for (;;)
		switch (ref->state) {
		case WT_REF_DISK:
			/* The page isn't in memory, request it be read. */
			WT_RET(__wt_cache_read_serial(
			    session, parent, ref, dsk_verify));
			break;
		case WT_REF_LOCKED:
			/*
			 * The page is being considered for eviction -- wait
			 * for that to be resolved.
			 */
			__wt_yield();
			break;
		case WT_REF_MEM:
			/*
			 * The page is in memory: get a hazard reference, update
			 * the page's LRU and return.  The expected reason we
			 * can't get a hazard reference is because the page is
			 * being evicted; yield and try again.
			 */
			if (__wt_hazard_set(session, ref
#ifdef HAVE_DIAGNOSTIC
			    , file, line
#endif
			    )) {
				ref->page->read_gen =
				    __wt_cache_read_gen(session);
				return (0);
			}
			__wt_yield();
			break;
		default:
			WT_FAILURE_RET(session, WT_ERROR, "invalid page state");
		}
	/* NOTREACHED */
}

/*
 * __wt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_page_inmem(WT_SESSION_IMPL *session,
    WT_PAGE *parent, WT_REF *parent_ref, WT_PAGE_DISK *dsk, WT_PAGE **pagep)
{
	WT_PAGE *page;
	int ret;

	WT_ASSERT_RET(session, dsk->u.entries > 0);

	*pagep = NULL;

	/*
	 * Allocate and initialize the WT_PAGE.
	 * Set the LRU so the page is not immediately selected for eviction.
	 */
	WT_RET(__wt_calloc_def(session, 1, &page));
	page->type = dsk->type;
	page->parent = parent;
	page->parent_ref = parent_ref;
	page->dsk = dsk;
	page->read_gen = __wt_cache_read_gen(session);

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		page->u.col_leaf.recno = dsk->recno;
		WT_ERR(__inmem_col_fix(page));
		break;
	case WT_PAGE_COL_INT:
		page->u.col_int.recno = dsk->recno;
		WT_ERR(__inmem_col_int(session, page));
		break;
	case WT_PAGE_COL_VAR:
		page->u.col_leaf.recno = dsk->recno;
		WT_ERR(__inmem_col_var(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_ERR(__inmem_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__inmem_row_leaf(session, page));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	*pagep = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __inmem_col_fix --
 *	Build in-memory index for fixed-length column-store leaf pages.
 */
static int
__inmem_col_fix(WT_PAGE *page)
{
	WT_PAGE_DISK *dsk;

	dsk = page->dsk;

	page->u.col_leaf.bitf = WT_PAGE_DISK_BYTE(dsk);
	page->entries = dsk->u.entries;
	return (0);
}

/*
 * __inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static int
__inmem_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD *off_record;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;

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

	page->entries = dsk->u.entries;

	/* Column-store internal pages do not require a disk image. */
	__wt_free(session, page->dsk);

	return (0);
}

/*
 * __inmem_col_var --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static int
__inmem_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;
	unpack = &_unpack;

	/*
	 * Column-store page entries map one-to-one to the number of physical
	 * entries on the page (each physical entry is a data item).
	 */
	WT_RET(__wt_calloc_def(
	    session, (size_t)dsk->u.entries, &page->u.col_leaf.d));

	/*
	 * Walk the page, building references: the page contains unsorted value
	 * items.  The value items are on-page (WT_CELL_VALUE), overflow items
	 * (WT_CELL_VALUE_OVFL) or deleted items (WT_CELL_DEL).
	 */
	cip = page->u.col_leaf.d;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		(cip++)->__value = WT_DISK_OFFSET(dsk, cell);
	}

	page->entries = dsk->u.entries;
	return (0);
}

/*
 * __inmem_row_int --
 *	Build in-memory index for row-store internal pages.
 */
static int
__inmem_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_BUF *current, *last, *tmp;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE_DISK *dsk;
	WT_ROW_REF *rref;
	uint32_t i, nindx, prefix;
	int found_ovfl, ret;
	void *huffman;

	btree = session->btree;
	current = last = NULL;
	unpack = &_unpack;
	dsk = page->dsk;
	found_ovfl = ret = 0;
	huffman = btree->huffman_key;

	WT_ERR(__wt_scr_alloc(session, 0, &current));
	WT_ERR(__wt_scr_alloc(session, 0, &last));

	/*
	 * Internal row-store page entries map one-to-two to the number of
	 * physical entries on the page (each physical entry is a data item
	 * and offset object).
	 */
	nindx = dsk->u.entries / 2;
	WT_RET((__wt_calloc_def(session, (size_t)nindx, &page->u.row_int.t)));

	/*
	 * Set the number of elements now -- we're about to allocate memory,
	 * and if we fail in the middle of the page, we want to discard that
	 * memory properly.
	 */
	page->entries = nindx;

	/*
	 * Walk the page, instantiating keys: the page contains sorted key and
	 * offpage-reference pairs.  Keys are row store internal pages with
	 * on-page/overflow (WT_CELL_KEY/KEY_OVFL) items, and offpage references
	 * are WT_CELL_OFF items.
	 */
	rref = page->u.row_int.t;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_KEY:
			break;
		case WT_CELL_OFF:
			WT_ROW_REF_ADDR(rref) = unpack->off.addr;
			WT_ROW_REF_SIZE(rref) = unpack->off.size;
			++rref;
			continue;
		WT_ILLEGAL_FORMAT(session);
		}

		/*
		 * We can discard the underlying disk page if we don't have any
		 * overflow keys.
		 */
		if (unpack->ovfl)
			found_ovfl = 1;

		/*
		 * If Huffman decoding is required or it's an overflow record,
		 * use the heavy-weight __wt_cell_unpack_copy() call to build
		 * the key.  Else, we can do it faster internally as we don't
		 * have to shuffle memory around as much.
		 */
		prefix = unpack->prefix;
		if (huffman != NULL || unpack->ovfl) {
			WT_RET(__wt_cell_unpack_copy(session, unpack, current));

			/*
			 * If there's a prefix, make sure there's enough buffer
			 * space, then shift the decoded data past the prefix
			 * and copy the prefix into place.
			 */
			if (prefix != 0) {
				WT_ERR(__wt_buf_grow(
				    session, current, prefix + current->size));
				memmove((uint8_t *)current->data +
				    prefix, current->data, current->size);
				memcpy(
				    (void *)current->data, last->data, prefix);
				current->size += prefix;
			}
		} else {
			/*
			 * Get the cell's data/length and make sure we have
			 * enough buffer space.
			 */
			WT_ERR(__wt_buf_grow(
			    session, current, prefix + unpack->size));

			/* Copy the prefix then the data into place. */
			if (prefix != 0)
				memcpy((void *)
				    current->data, last->data, prefix);
			memcpy((uint8_t *)
			    current->data + prefix, unpack->data, unpack->size);
			current->size = prefix + unpack->size;
		}

		/*
		 * Allocate and initialize the instantiated key.
		 *
		 * If the key is an overflow item, we'll retain the disk image,
		 * and will need a reference to it during reconciliation.
		 */
		WT_ERR(__wt_row_ikey_alloc(session,
		    unpack->ovfl ? WT_DISK_OFFSET(dsk, cell) : 0,
		    current->data, current->size, (WT_IKEY **)&rref->key));

		/*
		 * Swap buffers if it's not an overflow key, we have a new
		 * prefix-compressed page.
		 */
		if (!unpack->ovfl) {
			tmp = last;
			last = current;
			current = tmp;
		}
	}

	if (!found_ovfl)
		__wt_free(session, page->dsk);

err:	if (current != NULL)
		__wt_scr_release(&current);
	if (last != NULL)
		__wt_scr_release(&last);
	return (ret);
}

/*
 * __inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__inmem_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i, nindx;

	dsk = page->dsk;
	unpack = &_unpack;

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
	 * (WT_CELL_VALUE) or overflow (WT_CELL_VALUE_OVFL) item.
	 */
	nindx = 0;
	rip = page->u.row_leaf.d;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_KEY:
			++nindx;
			if (rip->key != NULL)
				++rip;
			rip->key = cell;
			break;
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
			break;
		WT_ILLEGAL_FORMAT(session);
		}
	}

	page->entries = nindx;

	return (__inmem_row_leaf_keys(session, page));
}

/*
 * __inmem_row_leaf_keys --
 *	Instantiate the interesting keys for random search of a page.
 */
static int
__inmem_row_leaf_keys(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_ROW *rip;
	uint32_t i;
	uint8_t *list;
	int ret;

	btree = session->btree;
	ret = 0;

	/*
	 * Row-store leaf pages are written as one big prefix-compressed chunk,
	 * that is, only the first key on the page is not prefix-compressed, and
	 * to instantiate the last key on the page, you have to take the first
	 * key on the page and roll it forward to the end of the page.  We don't
	 * want to do that on every page access, of course, so we instantiate a
	 * set of keys, essentially creating prefix chunks on the page, where we
	 * can roll forward from the closest, previous, instantiated key.  The
	 * complication is that not all keys on a page are equal: we're doing a
	 * binary search on the  page, which means there are keys we look at a
	 * lot (every time we search the page), and keys we never look at unless
	 * they are actually being searched for.  This function figures out the
	 * "interesting" keys on a page, and then we sequentially walk that list
	 * instantiating those keys.
	 *
	 * Allocate a bit array and figure out the set of "interesting" keys,
	 * marking up the array.
	 */
	WT_RET(__bit_alloc(session, page->entries, &list));
	__inmem_row_leaf_slots(list, 0, page->entries, btree->key_gap);

	/* Instantiate the keys. */
	for (rip = page->u.row_leaf.d, i = 0; i < page->entries; ++rip, ++i)
		if (__bit_test(list, i))
			WT_ERR(__wt_row_key(session, page, rip, NULL));

err:	__wt_free(session, list);
	return (ret);
}

/*
 * __inmem_row_leaf_slots --
 *	Figure out the interesting slots of a page for random search, up to
 * the specified depth.
 */
static void
__inmem_row_leaf_slots(
    uint8_t *list, uint32_t base, uint32_t entries, uint32_t gap)
{
	uint32_t indx, limit;

	if (entries < gap)
		return;

	/*
	 * !!!
	 * Don't clean this code up -- it deliberately looks like the binary
	 * search code.
	 *
	 * !!!
	 * There's got to be a function that would give me this information, I
	 * don't see any reason we can't just do this recursively.
	 */
	limit = entries;
	indx = base + (limit >> 1);
	__bit_set(list, indx);

	__inmem_row_leaf_slots(list, base, limit >> 1, gap);

	base = indx + 1;
	--limit;
	__inmem_row_leaf_slots(list, base, limit >> 1, gap);
}
