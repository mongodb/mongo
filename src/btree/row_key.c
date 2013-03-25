/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __inmem_row_leaf_slots(uint8_t *, uint32_t, uint32_t, uint32_t);

/*
 * __wt_row_leaf_keys --
 *	Instantiate the interesting keys for random search of a page.
 */
int
__wt_row_leaf_keys(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_ROW *rip;
	uint32_t gap, i;

	btree = S2BT(session);

	if (page->entries == 0) {			/* Just checking... */
		F_SET_ATOMIC(page, WT_PAGE_BUILD_KEYS);
		return (0);
	}

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
	WT_RET(__wt_scr_alloc(
	    session, (uint32_t)__bitstr_size(page->entries), &tmp));

	if ((gap = btree->key_gap) == 0)
		gap = 1;
	__inmem_row_leaf_slots(tmp->mem, 0, page->entries, gap);

	/* Instantiate the keys. */
	for (rip = page->u.row.d, i = 0; i < page->entries; ++rip, ++i)
		if (__bit_test(tmp->mem, i))
			WT_ERR(__wt_row_key_copy(session, page, rip, NULL));

	F_SET_ATOMIC(page, WT_PAGE_BUILD_KEYS);

err:	__wt_scr_free(&tmp);
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
	 * There's got to be a function that would give me this information, but
	 * I don't see any performance reason we can't just do this recursively.
	 */
	limit = entries;
	indx = base + (limit >> 1);
	__bit_set(list, indx);

	__inmem_row_leaf_slots(list, base, limit >> 1, gap);

	base = indx + 1;
	--limit;
	__inmem_row_leaf_slots(list, base, limit >> 1, gap);
}

/*
 * __wt_row_key_copy --
 *	Copy an on-page key into a return buffer, or, if no return buffer
 * is specified, instantiate the key into the in-memory page.
 */
int
__wt_row_key_copy(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg, WT_ITEM *retb)
{
	enum { FORWARD, BACKWARD } direction;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_ROW *rip;
	int is_local, slot_offset;
	void *key;

	rip = rip_arg;
	unpack = &_unpack;

	/* If the caller didn't pass us a buffer, create one. */
	is_local = 0;
	if (retb == NULL) {
		is_local = 1;
		WT_ERR(__wt_scr_alloc(session, 0, &retb));
	}

	direction = BACKWARD;
	for (slot_offset = 0;;) {
		key = WT_ROW_KEY_COPY(rip);

		/*
		 * Key copied.
		 *
		 * If another thread instantiated the key while we were doing
		 * that, we don't have any work to do.  Figure this out using
		 * the key's value:
		 *
		 * If the key points off-page, another thread updated the key,
		 * we can just use it.
		 *
		 * If the key points on-page, we have a copy of a WT_CELL value
		 * that can be processed, regardless of what any other thread is
		 * doing.
		 *
		 * Overflow keys are not prefix-compressed, we don't want to
		 * read/write them during reconciliation simply because their
		 * prefix might change.  That means we can't use instantiated
		 * overflow keys to figure out the prefix for other keys,
		 * specifically, in this code when we're looking for a key we
		 * can roll-forward to figure out the target key's prefix,
		 * instantiated overflow keys aren't useful.
		 *
		 * 1: the test for an on/off page reference.
		 */
		if (__wt_off_page(page, key)) {
			ikey = key;

			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, or if
			 * it's an overflow key or not, it's what we wanted.
			 * Take a copy and wrap up.
			 */
			if (slot_offset == 0) {
				WT_ERR(__wt_buf_set(session,
				    retb, WT_IKEY_DATA(ikey), ikey->size));
				break;
			}

			__wt_cell_unpack(
			    WT_PAGE_REF_OFFSET(page, ikey->cell_offset),
			    unpack);

			/*
			 * If we wanted a different key and this key is an
			 * overflow key:
			 *	If we're rolling backward, this key is useless
			 * to us because it doesn't have a valid prefix: keep
			 * rolling backward.
			 *	If we're rolling forward, there's no work to be
			 * done because prefixes skip overflow keys: keep
			 * rolling forward.
			 */
			if (unpack->ovfl)
				goto next;

			/*
			 * If we wanted a different key and this key is not an
			 * overflow key, it has a valid prefix, we can use it.
			 *	If rolling backward, take a copy of the key and
			 * switch directions, we can roll forward from this key.
			 *	If rolling forward, replace the key we've been
			 * building with this key, it's what we would have built
			 * anyway.
			 * In short: if it's not an overflow key, take a copy
			 * and roll forward.
			 */
			WT_ERR(__wt_buf_set(
			    session, retb, WT_IKEY_DATA(ikey), ikey->size));
			direction = FORWARD;
			goto next;
		}

		/* Unpack the key's cell. */
		__wt_cell_unpack(key, unpack);

		/* 2: the test for an on-page reference to an overflow key. */
		if (unpack->type == WT_CELL_KEY_OVFL) {
			/*
			 * If this is the key we wanted from the start, we don't
			 * care if it's an overflow key, get a copy and wrap up.
			 */
			if (slot_offset == 0) {
				WT_ERR(__wt_cell_unpack_copy(
				    session, unpack, retb));
				break;
			}

			/*
			 * If we wanted a different key and this key is an
			 * overflow key:
			 *	If we're rolling backward, this key is useless
			 * to us because it doesn't have a valid prefix: keep
			 * rolling backward.
			 *	If we're rolling forward, there's no work to be
			 * done because prefixes skip overflow keys: keep
			 * rolling forward.
			 */
			goto next;
		}

		/*
		 * 3: the test for an on-page reference to a key that isn't
		 * prefix compressed.
		 */
		if (unpack->prefix == 0) {
			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, it's
			 * what we want.  Take a copy and wrap up.
			 *
			 * If we wanted a different key, this key has a valid
			 * prefix we can use it.
			 *	If rolling backward, take a copy of the key and
			 * switch directions, we can roll forward from this key.
			 *	If rolling forward there's a bug, we should have
			 * found this key while rolling backwards and switched
			 * directions then.
			 */
			WT_ERR(__wt_cell_unpack_copy(session, unpack, retb));
			if (slot_offset == 0)
				break;

			WT_ASSERT(session, direction == BACKWARD);
			direction = FORWARD;
			goto next;
		}

		/*
		 * 4: an on-page reference to a key that's prefix compressed.
		 *	If rolling backward, keep looking for something we can
		 * use.
		 *	If rolling forward, build the full key and keep rolling
		 * forward.
		 */
		if (direction == FORWARD) {
			/*
			 * Get a reference to the current key's bytes;
			 * Ensure the buffer can hold the key's bytes plus the
			 *    prefix (and also setting the final buffer size);
			 * Append the key to the prefix (already in the buffer).
			 */
			if (tmp == NULL)
				WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__wt_cell_unpack_ref(session, unpack, tmp));
			WT_ERR(__wt_buf_initsize(
			    session, retb, tmp->size + unpack->prefix));
			memcpy((uint8_t *)
			    retb->data + unpack->prefix, tmp->data, tmp->size);

			if (slot_offset == 0)
				break;
		}

next:		switch (direction) {
		case  BACKWARD:
			--rip;
			++slot_offset;
			break;
		case FORWARD:
			++rip;
			--slot_offset;
			break;
		}
	}

	__wt_scr_free(&tmp);

	/*
	 * If a return buffer was specified, the caller just wants a copy and
	 * no further work is needed.
	 */
	if (!is_local)
		return (0);

	/* If still needed, instantiate the key. */
	key = WT_ROW_KEY_COPY(rip_arg);
	if (!__wt_off_page(page, key)) {
		WT_ERR(__wt_row_ikey_alloc(session,
		    WT_PAGE_DISK_OFFSET(page, key),
		    retb->data, retb->size, &ikey));

		/*
		 * Serialize the swap of the key into place.  If we succeed,
		 * update the page's memory footprint; if we fail, free the
		 * WT_IKEY structure.
		 */
		if (WT_ATOMIC_CAS(WT_ROW_KEY_COPY(rip), key, ikey))
			__wt_cache_page_inmem_incr(
			    session, page, sizeof(WT_IKEY) + ikey->size);
		else
			__wt_free(session, ikey);
	}

	__wt_scr_free(&retb);

	return (ret);

err:	if (is_local && retb != NULL)
		__wt_scr_free(&retb);
	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_row_value --
 *	Return a pointer to the value cell for a row-store leaf page key, or
 * NULL if there isn't one.
 */
WT_CELL *
__wt_row_value(WT_PAGE *page, WT_ROW *rip)
{
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	u_int type;

	cell = WT_ROW_KEY_COPY(rip);
	/*
	 * Key copied.
	 *
	 * Cell now either references a WT_IKEY structure with a cell offset,
	 * or references the on-page key WT_CELL.  Both can be processed
	 * regardless of what other threads are doing.  If it's the former,
	 * use it to get the latter.
	 */
	if (__wt_off_page(page, cell))
		cell = WT_PAGE_REF_OFFSET(page, ((WT_IKEY *)cell)->cell_offset);

	/*
	 * Row-store leaf pages may have a single data cell between each key, or
	 * keys may be adjacent (when the data cell is empty).  Move to the next
	 * cell and check its type.
	 *
	 * One special case: if the last key on a page is a key without a value,
	 * don't walk off the end of the page: the size of the underlying disk
	 * image is exact, which means the end of the last cell on the page plus
	 * the length of the cell should be the byte immediately after the page
	 * disk image.
	 */
	__wt_cell_unpack(cell, &unpack);
	cell = (WT_CELL *)((uint8_t *)cell + __wt_cell_total_len(&unpack));
	if (__wt_off_page(page, cell))
		return (NULL);

	type = __wt_cell_type(cell);
	return (type == WT_CELL_KEY || type == WT_CELL_KEY_OVFL ? NULL : cell);
}

/*
 * __wt_row_ikey_alloc --
 *	Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey_alloc(WT_SESSION_IMPL *session,
    uint32_t cell_offset, const void *key, uint32_t size, void *ikeyp)
{
	WT_IKEY *ikey;

	/*
	 * Allocate the WT_IKEY structure and room for the value, then copy
	 * the value into place.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_IKEY) + size, &ikey));
	ikey->size = size;
	ikey->cell_offset = cell_offset;
	memcpy(WT_IKEY_DATA(ikey), key, size);

	*(WT_IKEY **)ikeyp = ikey;
	return (0);
}
