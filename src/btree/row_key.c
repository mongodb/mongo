/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
			WT_ERR(__wt_row_leaf_key_work(
			    session, page, rip, NULL, 1));

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
 * __wt_row_leaf_key_copy --
 *	Get a copy of a row-store leaf-page key.
 */
int
__wt_row_leaf_key_copy(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg, WT_ITEM *retb)
{
	WT_RET(__wt_row_leaf_key_work(session, page, rip_arg, retb, 0));

	/* The return buffer may only hold a reference to a key, copy it. */
	if (!WT_DATA_IN_ITEM(retb))
		WT_RET(__wt_buf_set(session, retb, retb->data, retb->size));

	return (0);
}

/*
 * __wt_row_leaf_key_work --
 *	Return a reference to, or copy of, a row-store leaf-page key.
 * Optionally instantiate the key into the in-memory page.
 */
int
__wt_row_leaf_key_work(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW *rip_arg, WT_ITEM *retb_arg, int instantiate)
{
	enum { FORWARD, BACKWARD } direction;
	WT_BTREE *btree;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(retb);
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_ROW *rip;
	int slot_offset;
	void *key;

	btree = S2BT(session);
	unpack = &_unpack;
	rip = rip_arg;

	/* If the caller didn't pass us a buffer, allocate a scratch one. */
	if ((retb = retb_arg) == NULL)
		WT_ERR(__wt_scr_alloc(session, 0, &retb));

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
off_page:		ikey = key;

			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, or if
			 * it's an overflow key or not, it's what we wanted.
			 * Take a copy and wrap up.
			 */
			if (slot_offset == 0) {
				retb->data = WT_IKEY_DATA(ikey);
				retb->size = ikey->size;

				/*
				 * The key is already instantiated, ignore the
				 * caller's suggestion.
				 */
				instantiate = 0;
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
			if (__wt_cell_type(WT_PAGE_REF_OFFSET(
			    page, ikey->cell_offset)) == WT_CELL_KEY_OVFL)
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
			retb->data = WT_IKEY_DATA(ikey);
			retb->size = ikey->size;
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
			 *
			 * Avoid racing with reconciliation deleting overflow
			 * keys.  Deleted overflow keys must be instantiated
			 * first, acquire the overflow lock and check.  Read
			 * the key if we still need to do so, but holding the
			 * overflow lock.  Note we are not using the version of
			 * the cell-data-ref calls that acquire the overflow
			 * lock and do a look-aside into the tracking cache:
			 * this is an overflow key, not a value, meaning it's
			 * instantiated before being deleted, not copied into
			 * the tracking cache.
			 */
			if (slot_offset == 0) {
				WT_ERR(__wt_readlock(
				    session, S2BT(session)->ovfl_lock));
				key = WT_ROW_KEY_COPY(rip);
				if (__wt_off_page(page, key)) {
					WT_ERR(__wt_rwunlock(session,
					    S2BT(session)->ovfl_lock));
					goto off_page;
				}
				ret = __wt_dsk_cell_data_ref(
				    session, WT_PAGE_ROW_LEAF, unpack, retb);
				WT_TRET(__wt_rwunlock(session,
				    S2BT(session)->ovfl_lock));
				WT_ERR(ret);
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
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, WT_PAGE_ROW_LEAF, unpack, retb));
			if (slot_offset == 0) {
				/*
				 * If we have an uncompressed, on-page key with
				 * no prefix, don't bother instantiating it,
				 * regardless of what our caller thought.  The
				 * memory cost is greater than the performance
				 * cost of finding the key each time we need it.
				 */
				if (btree->huffman_key == NULL)
					instantiate = 0;
				break;
			}

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
			/* Get a reference to the current key's bytes. */
			if (tmp == NULL)
				WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, WT_PAGE_ROW_LEAF, unpack, tmp));

			/*
			 * The return buffer may only hold a reference to a key,
			 * if we've not actually rolled forward until now.  Copy
			 * the data into real buffer memory.
			 */
			if (retb->data != retb->mem)
				WT_ERR(__wt_buf_set(
				    session, retb, retb->data, retb->size));

			/*
			 * Ensure the buffer can hold the key's bytes plus the
			 *    prefix (and also setting the final buffer size);
			 * Append the key to the prefix (already in the buffer).
			 */
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

	/* Optionally instantiate the key. */
	if (instantiate) {
		key = WT_ROW_KEY_COPY(rip_arg);
		if (!__wt_off_page(page, key)) {
			WT_ERR(__wt_row_ikey(session,
			    WT_PAGE_DISK_OFFSET(page, key),
			    retb->data, retb->size, &ikey));

			/*
			 * Serialize the swap of the key into place: on success,
			 * update the page's memory footprint, on failure, free
			 * the allocated memory.
			 */
			if (WT_ATOMIC_CAS(WT_ROW_KEY_COPY(rip), key, ikey))
				__wt_cache_page_inmem_incr(session,
				    page, sizeof(WT_IKEY) + ikey->size);
			else
				__wt_free(session, ikey);
		}
	}

err:	__wt_scr_free(&tmp);
	if (retb != retb_arg)
		__wt_scr_free(&retb);

	return (ret);
}

/*
 * __wt_row_ikey_incr --
 *	Instantiate a key in a WT_IKEY structure and increment the page's
 * memory footprint.
 */
int
__wt_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page,
    uint32_t cell_offset, const void *key, size_t size, void *ikeyp)
{
	WT_RET(__wt_row_ikey(session, cell_offset, key, size, ikeyp));

	__wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + size);

	return (0);
}

/*
 * __wt_row_ikey --
 *	Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey(WT_SESSION_IMPL *session,
    uint32_t cell_offset, const void *key, size_t size, void *ikeyp)
{
	WT_IKEY *ikey;

	/*
	 * Allocate memory for the WT_IKEY structure and the key, then copy
	 * the key into place.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_IKEY) + size, &ikey));
	ikey->size = WT_STORE_SIZE(size);
	ikey->cell_offset = cell_offset;
	memcpy(WT_IKEY_DATA(ikey), key, size);

	*(WT_IKEY **)ikeyp = ikey;
	return (0);
}
