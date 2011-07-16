/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_row_key --
 *	Copy an on-page key into a return buffer, or, if no return buffer
 * is specified, instantiate the key into the in-memory page.
 */
int
__wt_row_key(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg, WT_BUF *retb)
{
	enum { FORWARD, BACKWARD } direction;
	WT_BUF *tmp;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_ROW *rip;
	int is_local, ret, slot_offset;
	void *key;

	rip = rip_arg;
	tmp = NULL;
	unpack = &_unpack;

	/*
	 * If the caller didn't pass us a buffer, create one.  We don't use
	 * an existing buffer because the memory will be attached to a page
	 * for semi-permanent use, and using an existing buffer might waste
	 * memory if the one allocated from the pool was larger than needed.
	 */
	is_local = 0;
	if (retb == NULL) {
		is_local = 1;
		WT_ERR(__wt_scr_alloc(session, 0, &retb));
	}

	direction = BACKWARD;
	for (slot_offset = 0;;) {
		/*
		 * Multiple threads of control may be searching this page, which
		 * means the key may change underfoot, and here's where it gets
		 * tricky: first, copy the key.
		 */
		key = rip->key;

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
		 * Row-store internal and leaf pages have a bit array with one
		 * element for each of the original keys on the page.  If that
		 * bit is set, the instantiated key was an overflow key, else,
		 * it wasn't.
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
			    WT_REF_OFFSET(page, ikey->cell_offset), unpack);

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
			 * Get a copy of the current key;
			 * Ensure the buffer can hold the key plus the prefix;
			 * Append the key to the prefix (already in the buffer);
			 * Set the final size of the key.
			 */
			if (tmp == NULL)
				WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__wt_cell_unpack_copy(session, unpack, tmp));
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

	if (tmp != NULL)
		__wt_scr_release(&tmp);

	/*
	 * If a return buffer was specified, the caller just wants a copy and
	 * no further work is needed.
	 */
	if (!is_local)
		return (0);

	/*
	 * Allocate and initialize a WT_IKEY structure, we're instantiating
	 * this key.
	 */
	WT_ERR(__wt_row_ikey_alloc(session,
	    WT_DISK_OFFSET(page->dsk, rip_arg->key),
	    retb->data, retb->size, &ikey));

	/* Serialize the swap of the key into place. */
	ret = __wt_row_key_serial(session, page, rip_arg, ikey);

	/* Free the WT_IKEY structure if the workQ didn't use it for the key. */
	if (rip_arg->key != ikey)
		__wt_sb_decrement(session, ikey->sb);

	__wt_scr_release(&retb);

	return (ret);

err:	if (is_local && retb != NULL)
		__wt_scr_release(&retb);
	if (tmp != NULL)
		__wt_scr_release(&tmp);
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
	WT_CELL_UNPACK *unpack, _unpack;

	unpack = &_unpack;

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first field of each
	 * is a void *.
	 *
	 * Multiple threads of control may be searching this page, which means
	 * the key may change underfoot, and here's where it gets tricky: first,
	 * copy the key.
	 */
	cell = rip->key;

	/*
	 * Key copied.
	 *
	 * Now, cell either references a WT_IKEY structure that has a value-cell
	 * offset, or references the on-page key WT_CELL, and we can walk past
	 * that to find the value WT_CELL.  Both can be processed regardless of
	 * what other threads are doing.
	 */
	if (__wt_off_page(page, cell))
		cell = WT_REF_OFFSET(page, ((WT_IKEY *)cell)->cell_offset);

	/*
	 * Row-store leaf pages may have a single data cell between each key, or
	 * keys may be adjacent (when the data cell is empty).  Move to the next
	 * key.  The page reconciliation code guarantees there is always a key
	 * cell after an empty data cell, so this is safe.
	 */
	__wt_cell_unpack(cell, unpack);
	cell = (WT_CELL *)((uint8_t *)cell + unpack->len);
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
__wt_row_ikey_alloc(WT_SESSION_IMPL *session,
    uint32_t cell_offset, const void *key, uint32_t size, WT_IKEY **ikeyp)
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
	ikey->cell_offset = cell_offset;
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

	__wt_row_key_unpack(session, &page, &rip, &ikey);

	/*
	 * We don't care about the page's write generation -- there's a simpler
	 * test, if the key we're interested in still needs to be instantiated,
	 * because it can only be in one of two states.
	 *
	 * Passed both WT_ROW_REF and WT_ROW structures; the first field of each
	 * is a void *.
	 */
	if (!__wt_off_page(page, rip->key))
		rip->key = ikey;

	__wt_session_serialize_wrapup(session, NULL, 0);
	return (0);
}
