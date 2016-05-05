/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
	WT_DECL_ITEM(key);
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_ROW *rip;
	uint32_t gap, i;

	btree = S2BT(session);

	if (page->pg_row_entries == 0) {		/* Just checking... */
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
	WT_RET(__wt_scr_alloc(session, 0, &key));
	WT_RET(__wt_scr_alloc(session,
	    (uint32_t)__bitstr_size(page->pg_row_entries), &tmp));
	memset(tmp->mem, 0, tmp->memsize);

	if ((gap = btree->key_gap) == 0)
		gap = 1;
	__inmem_row_leaf_slots(tmp->mem, 0, page->pg_row_entries, gap);

	/* Instantiate the keys. */
	for (rip = page->pg_row_d, i = 0; i < page->pg_row_entries; ++rip, ++i)
		if (__bit_test(tmp->mem, i))
			WT_ERR(__wt_row_leaf_key_work(
			    session, page, rip, key, true));

	F_SET_ATOMIC(page, WT_PAGE_BUILD_KEYS);

err:	__wt_scr_free(session, &key);
	__wt_scr_free(session, &tmp);
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
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_ITEM *key)
{
	WT_RET(__wt_row_leaf_key(session, page, rip, key, false));

	/* The return buffer may only hold a reference to a key, copy it. */
	if (!WT_DATA_IN_ITEM(key))
		WT_RET(__wt_buf_set(session, key, key->data, key->size));

	return (0);
}

/*
 * __wt_row_leaf_key_work --
 *	Return a reference to, a row-store leaf-page key, optionally instantiate
 * the key into the in-memory page.
 */
int
__wt_row_leaf_key_work(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW *rip_arg, WT_ITEM *keyb, bool instantiate)
{
	enum { FORWARD, BACKWARD } direction;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_ROW *rip, *jump_rip;
	size_t size;
	u_int last_prefix;
	int jump_slot_offset, slot_offset;
	void *copy;
	const void *p;

	/*
	 * !!!
	 * It is unusual to call this function: most code should be calling the
	 * front-end, __wt_row_leaf_key, be careful if you're calling this code
	 * directly.
	 */

	btree = S2BT(session);
	unpack = &_unpack;
	rip = rip_arg;

	jump_rip = NULL;
	jump_slot_offset = 0;
	last_prefix = 0;

	p = NULL;			/* -Werror=maybe-uninitialized */
	size = 0;			/* -Werror=maybe-uninitialized */

	direction = BACKWARD;
	for (slot_offset = 0;;) {
		if (0) {
switch_and_jump:	/* Switching to a forward roll. */
			WT_ASSERT(session, direction == BACKWARD);
			direction = FORWARD;

			/* Skip list of keys with compatible prefixes. */
			rip = jump_rip;
			slot_offset = jump_slot_offset;
		}
		copy = WT_ROW_KEY_COPY(rip);

		/*
		 * Figure out what the key looks like.
		 */
		(void)__wt_row_leaf_key_info(
		    page, copy, &ikey, &cell, &p, &size);

		/* 1: the test for a directly referenced on-page key. */
		if (cell == NULL) {
			keyb->data = p;
			keyb->size = size;

			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, or if
			 * it's an overflow key or not, it's what we wanted.
			 * This shouldn't normally happen, the fast-path code
			 * that front-ends this function will have figured it
			 * out before we were called.
			 *
			 * The key doesn't need to be instantiated, skip past
			 * that test.
			 */
			if (slot_offset == 0)
				goto done;

			/*
			 * This key is not an overflow key by definition and
			 * isn't compressed in any way, we can use it to roll
			 * forward.
			 *	If rolling backward, switch directions.
			 *	If rolling forward: there's a bug somewhere,
			 * we should have hit this key when rolling backward.
			 */
			goto switch_and_jump;
		}

		/* 2: the test for an instantiated off-page key. */
		if (ikey != NULL) {
			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, or if
			 * it's an overflow key or not, it's what we wanted.
			 * Take a copy and wrap up.
			 *
			 * The key doesn't need to be instantiated, skip past
			 * that test.
			 */
			if (slot_offset == 0) {
				keyb->data = p;
				keyb->size = size;
				goto done;
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
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
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
			keyb->data = p;
			keyb->size = size;
			direction = FORWARD;
			goto next;
		}

		/*
		 * It must be an on-page cell, unpack it.
		 */
		__wt_cell_unpack(cell, unpack);

		/* 3: the test for an on-page reference to an overflow key. */
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
				WT_ERR(
				    __wt_readlock(session, btree->ovfl_lock));
				copy = WT_ROW_KEY_COPY(rip);
				if (!__wt_row_leaf_key_info(page, copy,
				    NULL, &cell, &keyb->data, &keyb->size)) {
					__wt_cell_unpack(cell, unpack);
					ret = __wt_dsk_cell_data_ref(session,
					    WT_PAGE_ROW_LEAF, unpack, keyb);
				}
				WT_TRET(
				    __wt_readunlock(session, btree->ovfl_lock));
				WT_ERR(ret);
				break;
			}

			/*
			 * If we wanted a different key:
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
		 * 4: the test for an on-page reference to a key that isn't
		 * prefix compressed.
		 */
		if (unpack->prefix == 0) {
			/*
			 * The only reason to be here is a Huffman encoded key,
			 * a non-encoded key with no prefix compression should
			 * have been directly referenced, and we should not have
			 * needed to unpack its cell.
			 */
			WT_ASSERT(session, btree->huffman_key != NULL);

			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, it's
			 * what we want.  Take a copy and wrap up.
			 *
			 * If we wanted a different key, this key has a valid
			 * prefix, we can use it.
			 *	If rolling backward, take a copy of the key and
			 * switch directions, we can roll forward from this key.
			 *	If rolling forward there's a bug, we should have
			 * found this key while rolling backwards and switched
			 * directions then.
			 *
			 * The key doesn't need to be instantiated, skip past
			 * that test.
			 */
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, WT_PAGE_ROW_LEAF, unpack, keyb));
			if (slot_offset == 0)
				goto done;
			goto switch_and_jump;
		}

		/*
		 * 5: an on-page reference to a key that's prefix compressed.
		 *	If rolling backward, keep looking for something we can
		 * use.
		 *	If rolling forward, build the full key and keep rolling
		 * forward.
		 */
		if (direction == BACKWARD) {
			/*
			 * If there's a set of keys with identical prefixes, we
			 * don't want to instantiate each one, the prefixes are
			 * all the same.
			 *
			 * As we roll backward through the page, track the last
			 * time the prefix decreased in size, so we can start
			 * with that key during our roll-forward.  For a page
			 * populated with a single key prefix, we'll be able to
			 * instantiate the key we want as soon as we find a key
			 * without a prefix.
			 */
			if (slot_offset == 0)
				last_prefix = unpack->prefix;
			if (slot_offset == 0 || last_prefix > unpack->prefix) {
				jump_rip = rip;
				jump_slot_offset = slot_offset;
				last_prefix = unpack->prefix;
			}
		}
		if (direction == FORWARD) {
			/*
			 * Get a reference to the current key's bytes.  Usually
			 * we want bytes from the page, fast-path that case.
			 */
			if (btree->huffman_key == NULL) {
				p = unpack->data;
				size = unpack->size;
			} else {
				if (tmp == NULL)
					WT_ERR(
					__wt_scr_alloc(session, 0, &tmp));
				WT_ERR(__wt_dsk_cell_data_ref(
				    session, WT_PAGE_ROW_LEAF, unpack, tmp));
				p = tmp->data;
				size = tmp->size;
			}

			/*
			 * Grow the buffer as necessary as well as ensure data
			 * has been copied into local buffer space, then append
			 * the suffix to the prefix already in the buffer.
			 *
			 * Don't grow the buffer unnecessarily or copy data we
			 * don't need, truncate the item's data length to the
			 * prefix bytes.
			 */
			keyb->size = unpack->prefix;
			WT_ERR(__wt_buf_grow(session, keyb, keyb->size + size));
			memcpy((uint8_t *)keyb->data + keyb->size, p, size);
			keyb->size += size;

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

	/*
	 * Optionally instantiate the key: there's a cost to figuring out a key
	 * value in a leaf page with prefix-compressed or Huffman encoded keys,
	 * amortize the cost by instantiating a copy of the calculated key in
	 * allocated memory.  We don't instantiate keys when pages are first
	 * brought into memory because it's wasted effort if the page is only
	 * read by a cursor in sorted order.  If, instead, the page is read by a
	 * cursor in reverse order, we immediately instantiate periodic keys for
	 * the page (otherwise the reverse walk would be insanely slow).  If,
	 * instead, the page is randomly searched, we instantiate keys as they
	 * are accessed (meaning, for example, as long as the binary search only
	 * touches one-half of the page, the only keys we instantiate will be in
	 * that half of the page).
	 */
	if (instantiate) {
		copy = WT_ROW_KEY_COPY(rip_arg);
		(void)__wt_row_leaf_key_info(
		    page, copy, &ikey, &cell, NULL, NULL);
		if (ikey == NULL) {
			WT_ERR(__wt_row_ikey_alloc(session,
			    WT_PAGE_DISK_OFFSET(page, cell),
			    keyb->data, keyb->size, &ikey));

			/*
			 * Serialize the swap of the key into place: on success,
			 * update the page's memory footprint, on failure, free
			 * the allocated memory.
			 */
			if (__wt_atomic_cas_ptr(
			    (void *)&WT_ROW_KEY_COPY(rip), copy, ikey))
				__wt_cache_page_inmem_incr(session,
				    page, sizeof(WT_IKEY) + ikey->size);
			else
				__wt_free(session, ikey);
		}
	}

done:
err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_row_ikey_alloc --
 *	Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey_alloc(WT_SESSION_IMPL *session,
    uint32_t cell_offset, const void *key, size_t size, WT_IKEY **ikeyp)
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
	*ikeyp = ikey;
	return (0);
}

/*
 * __wt_row_ikey_incr --
 *	Instantiate a key in a WT_IKEY structure and increment the page's
 * memory footprint.
 */
int
__wt_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page,
    uint32_t cell_offset, const void *key, size_t size, WT_REF *ref)
{
	WT_RET(__wt_row_ikey(session, cell_offset, key, size, ref));

	__wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + size);

	return (0);
}

/*
 * __wt_row_ikey --
 *	Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey(WT_SESSION_IMPL *session,
    uint32_t cell_offset, const void *key, size_t size, WT_REF *ref)
{
	WT_IKEY *ikey;

	WT_RET(__wt_row_ikey_alloc(session, cell_offset, key, size, &ikey));

#ifdef HAVE_DIAGNOSTIC
	{
	uintptr_t oldv;

	oldv = (uintptr_t)ref->ref_ikey;
	WT_DIAGNOSTIC_YIELD;

	/*
	 * We should never overwrite an instantiated key, and we should
	 * never instantiate a key after a split.
	 */
	WT_ASSERT(session, oldv == 0 || (oldv & WT_IK_FLAG) != 0);
	WT_ASSERT(session, ref->state != WT_REF_SPLIT);
	WT_ASSERT(session,
	    __wt_atomic_cas_ptr(&ref->ref_ikey, (WT_IKEY *)oldv, ikey));
	}
#else
	ref->ref_ikey = ikey;
#endif
	return (0);
}
