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
	uint32_t pfx;
	uint8_t type;
	int is_local, is_ovfl, ret, slot_offset;
	void *key;

	dsk = page->dsk;
	type = page->type;

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
	 * Passed both WT_ROW_REF and WT_ROW structures.  It's simpler to just
	 * step both pointers back through the structure, and use the page type
	 * when we actually indirect through one of them so we don't use the
	 * one that's pointing into random memory.
	 */
	is_ovfl = 0;
	direction = BACKWARD;
	for (slot_offset = 0, rref = row_arg, rip = row_arg;;) {
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
			ikey = key;
			if (slot_offset == 0 ||
			    !F_ISSET(ikey, WT_IKEY_OVERFLOW)) {
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
	 * If a return buffer was specified, the caller just wants a copy and
	 * no further work is needed.
	 */
	if (!is_local)
		return (0);

	/*
	 * Allocate and initialize a WT_IKEY structure, we're instantiating
	 * this key.
	 */
	WT_ERR(__wt_row_ikey_alloc(session, retb->data, retb->size, &ikey));
	if ((cell = __wt_row_value(page, row_arg)) == NULL)
		ikey->value_cell_offset = WT_IKEY_VALUE_EMPTY;
	else
		ikey->value_cell_offset = WT_PAGE_DISK_OFFSET(dsk, cell);
	if (is_ovfl)
		F_SET(ikey, WT_IKEY_OVERFLOW);
	__wt_buf_free(session, &tmp);

	/* Serialize the swap of the key into place. */
	__wt_row_key_serial(session, page, row_arg, ikey, ret);

	/* Free the WT_IKEY structure if the workQ didn't use it for the key. */
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
 * __wt_row_ikey_all --
 *	Instantiate all of the keys for a page.
 */
int
__wt_row_ikey_all(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_BUF *current, *last, *tmp;
	WT_IKEY *ikey;
	WT_ROW_REF *rref;
	uint32_t data_size, i, prefix;
	int is_ovfl, ret;
	void *data, *huffman;

	btree = session->btree;
	current = last = NULL;
	huffman = btree->huffman_key;
	ret = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &current));
	WT_ERR(__wt_scr_alloc(session, 0, &last));

	WT_ROW_REF_FOREACH(page, rref, i) {
		cell = rref->key;
		prefix = __wt_cell_prefix(cell);
		is_ovfl = __wt_cell_type_is_ovfl(cell) ? 1 : 0;

		/*
		 * Overflow keys are simple, and don't participate in prefix
		 * compression.
		 *
		 * If Huffman decoding, use the heavy-weight __wt_cell_copy()
		 * code to build the key, up to the prefix. Else, we can do
		 * it faster internally because we don't have to shuffle memory
		 * around as much.
		 */
		if (is_ovfl)
			WT_RET(__wt_cell_copy(session, cell, current));
		else if (huffman == NULL) {
			/*
			 * Get the cell's data/length and make sure we have
			 * enough buffer space.
			 */
			__wt_cell_data_and_len(cell, &data, &data_size);
			WT_ERR(__wt_buf_grow(
			    session, current, prefix + data_size));

			/* Copy the prefix then the data into place. */
			if (prefix != 0)
				memcpy((void *)
				    current->data, last->data, prefix);
			memcpy((uint8_t *)
			    current->data + prefix, data, data_size);
			current->size = prefix + data_size;
		} else {
			WT_RET(__wt_cell_copy(session, cell, current));

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
		}

		/*
		 * Instantiate the key -- the key has no data, and overflow
		 * doesn't matter, but might as well be consistent.
		 */
		WT_ERR(__wt_row_ikey_alloc(
		    session, current->data, current->size, &ikey));
		ikey->value_cell_offset = WT_IKEY_VALUE_EMPTY;
		if (is_ovfl)
			F_SET(ikey, WT_IKEY_OVERFLOW);

		/* Swap buffers. */
		if (!is_ovfl) {
			tmp = last;
			last = current;
			current = tmp;
		}
	}

err:	if (current != NULL)
		__wt_scr_release(&current);
	if (last != NULL)
		__wt_scr_release(&last);
	return (ret);
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

	__wt_row_key_unpack(session, page, rip, ikey);

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
