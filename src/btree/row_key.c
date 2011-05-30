/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"
#include "cell.i"

/*
 * __wt_row_key --
 *	Copy an on-page key into a return buffer, or, if no return buffer
 * is specified, instantiate the key into the in-memory page.
 */
int
__wt_row_key(
    WT_SESSION_IMPL *session, WT_PAGE *page, void *row_arg, WT_BUF *retb)
{
	enum { FORWARD, BACKWARD } direction;
	WT_BUF build, tmp;
	WT_CELL *cell;
	WT_IKEY *ikey;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	bitstr_t *ovfl;
	uint32_t pfx, slot;
	uint8_t type;
	int is_ovfl, is_local, ret, slot_offset;
	void *key;

	dsk = page->dsk;
	WT_CLEAR(build);
	WT_CLEAR(tmp);

	/*
	 * If the caller didn't pass us a buffer, create one.  We don't use
	 * an existing buffer because the memory will be attached to a page
	 * for semi-permanent use, and using an existing buffer might waste
	 * memory if the one allocated from the pool was larger than needed.
	 */
	is_local = 0;
	if (retb == NULL) {
		retb = &build;
		is_local = 1;
	}

	/*
	 * Set up references to the different page structures.
	 *
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	type = page->type;
	switch (page->type) {
	case WT_PAGE_ROW_INT:
		slot = WT_ROW_REF_SLOT(page, row_arg);
		ovfl = page->u.row_int.ovfl;
		break;
	case WT_PAGE_ROW_LEAF:
		slot = WT_ROW_SLOT(page, row_arg);
		ovfl = page->u.row_leaf.ovfl;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures.  It's simpler to just
	 * step both pointers back through the structure, and use the page type
	 * when we actually indirect through one of them so we don't use the
	 * one that's pointing into random memory.
	 */
	direction = BACKWARD;
	for (is_ovfl = slot_offset = 0, rref = row_arg, rip = row_arg;;) {
		/*
		 * Multiple threads of control may be searching this page, which
		 * means the key may change underfoot, and here's where it gets
		 * tricky: first, copy the key.
		 */
		key = type == WT_PAGE_ROW_INT ? rref->key : rip->key;

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
		 * specifially, in this code when we're looking for a key we
		 * can roll-forward to figure out the target key's prefix,
		 * instantiated overflow keys aren't useful.
		 *
		 * Row-store internal and leaf pages have a bit array with one
		 * element for each of the original keys on the page.  If that
		 * bit is set, the instantiated key was an overflow key, else,
		 * it wasn't.
		 *
		 * 1: the test for an on/off page reference.
		 */
		if (__wt_off_page(page, key)) {
			/*
			 * If this is the key we originally wanted, we don't
			 * care if we're rolling forward or backward, or if
			 * it's an overflow key or not, it's what we wanted.
			 * Take a copy and wrap up.
			 *
			 * If we wanted a different key and this key is not an
			 * overflow key, it has a valid prefix, we can use it.
			 *	If rolling backward, take a copy of the key and
			 * switch directions, we can roll forward from this key.
			 *	If rolling forward, replace the key we've been
			 * building with this key, it's what we would have built
			 * anyway.
			 * To summarize, in both cases, take a copy of the key
			 * and roll forward.
			 */
			if (slot_offset == 0 ||
			    ovfl == NULL || !bit_test(ovfl, slot)) {
				ikey = key;
				WT_ERR(__wt_buf_set(session,
				    retb, WT_IKEY_DATA(ikey), ikey->size));
				if (slot_offset == 0)
					break;

				direction = FORWARD;
				goto next;
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

		/* 2: the test for an on-page reference to an overflow key. */
		if (__wt_cell_type(key) == WT_CELL_KEY_OVFL) {
			/*
			 * If this is the key we wanted from the start, we don't
			 * care if it's an overflow key.  Flag the target key
			 * was an overflow key: the serialization function needs
			 * to know so it can update the overflow bit array.
			 */
			if (slot_offset == 0) {
				is_ovfl = 1;
				WT_ERR(__wt_cell_copy(session, key, retb));
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
		if ((pfx = __wt_cell_prefix(key)) == 0) {
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
			WT_ERR(__wt_cell_copy(session, key, retb));
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
			 * Get a copy of the current key;
			 * Ensure the buffer can hold the key plus the prefix;
			 * Append the key to the prefix (already in the buffer);
			 * Set the final size of the key.
			 */
			WT_ERR(__wt_cell_copy(session, key, &tmp));
			WT_ERR(
			    __wt_buf_initsize(session, retb, tmp.size + pfx));
			memcpy((uint8_t *)retb->data + pfx, tmp.data, tmp.size);

			if (slot_offset == 0)
				break;
		}

next:		switch (direction) {
		case  BACKWARD:
			--rref; --rip; ++slot_offset;
			break;
		case FORWARD:
			++rref; ++rip; --slot_offset;
			break;
		}
	}

	__wt_buf_free(session, &tmp);

	/*
	 * If a return buffer was specified, the caller just wants a copy,
	 * no further work is needed.
	 */
	if (!is_local)
		return (0);

	WT_ERR(__wt_row_ikey_alloc(session, retb->data, retb->size, &ikey));
	if ((cell = __wt_row_value(page, row_arg)) == NULL)
		ikey->value_cell_offset = WT_IKEY_VALUE_EMPTY;
	else
		ikey->value_cell_offset = WT_PAGE_DISK_OFFSET(dsk, cell);
	__wt_buf_free(session, &tmp);

	/* Serialize the swap of the key into place. */
	__wt_row_key_serial(
	    session, page, row_arg, ikey, is_ovfl ? ovfl : NULL, slot, ret);

	/* Free the allocated memory if the workQ didn't use it for the key. */
	if (((WT_ROW *)row_arg)->key != ikey)
		__wt_sb_decrement(session, ikey->sb);

	return (ret);

err:	__wt_buf_free(session, &build);
	__wt_buf_free(session, &tmp);
	return (ret);
}

/*
 * __wt_row_value --
 *	Return a pointer to the value cell for a row-store leaf page key, or
 * NULL if there isn't one.
 */
WT_CELL *
__wt_row_value(WT_PAGE *page, void *row_arg)
{
	WT_CELL *cell;
	WT_IKEY *ikey;
	void *key;

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first field of each
	 * is a void *.
	 *
	 * Multiple threads of control may be searching this page, which means
	 * the key may change underfoot, and here's where it gets tricky: first,
	 * copy the key.
	 */
	key = ((WT_ROW *)row_arg)->key;

	/*
	 * Key copied.
	 *
	 * Now, key either references a WT_IKEY structure that has a value-cell
	 * offset, or references the on-page key WT_CELL, and we can walk past
	 * that to find the value WT_CELL.  Both can be processed regardless of
	 * what other threads are doing.
	 */
	if (__wt_off_page(page, key)) {
		ikey = key;
		return (WT_IKEY_VALUE_EMPTY_ISSET(ikey) ?
		    NULL : WT_ROW_PTR(page, ikey->value_cell_offset));
	}

	/*
	 * Row-store leaf pages may have a single data cell between each key, or
	 * keys may be adjacent (when the data cell is empty).  Move to the next
	 * key.  The page reconciliation code guarantees there is always a key
	 * cell after an empty data cell, so this is safe.
	 */
	cell = __wt_cell_next(key);
	if (__wt_cell_type(cell) == WT_CELL_KEY ||
	    __wt_cell_type(cell) == WT_CELL_KEY_OVFL)
		return (NULL);
	return (cell);
}

/*
 * __wt_row_ikey_alloc --
 *	Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey_alloc(
    WT_SESSION_IMPL *session, const void *key, uint32_t size, WT_IKEY **ikeyp)
{
	WT_IKEY *ikey;
	WT_SESSION_BUFFER *sb;

	/*
	 * Allocate the WT_IKEY structure and room for the value, then copy
	 * the value into place.
	 */
	WT_RET(__wt_sb_alloc(session, sizeof(WT_IKEY) + size, &ikey, &sb));
	ikey->sb = sb;
	ikey->size = size;
	memcpy(WT_IKEY_DATA(ikey), key, size);

	*ikeyp = ikey;
	return (0);
}

/*
 * __wt_row_key_serial_func --
 *	Server function to instantiate a key during a row-store search.
 */
int
__wt_row_key_serial_func(WT_SESSION_IMPL *session)
{
	WT_IKEY *ikey;
	WT_PAGE *page;
	WT_ROW *rip;
	bitstr_t *ovfl;
	uint32_t slot;

	__wt_row_key_unpack(session, page, rip, ikey, ovfl, slot);

	/*
	 * We don't care about the page's write generation -- there's a simpler
	 * test, if the key we're interested in still needs to be instantiated,
	 * because it can only be in one of two states.
	 *
	 * Passed both WT_ROW_REF and WT_ROW structures; the first field of each
	 * is a void *.
	 */
	if (!__wt_off_page(page, rip->key)) {
		if (ovfl != NULL)		/* WRONG BUT GOING AWAY */
			bit_set(ovfl, (int)slot);
		rip->key = ikey;
	}
	__wt_session_serialize_wrapup(session, NULL, 0);
	return (0);
}
