/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __cursor_set_recno --
 *	The cursor value in the interface has to track the value in the
 * underlying cursor, update them in parallel.
 */
static inline void
__cursor_set_recno(WT_CURSOR_BTREE *cbt, uint64_t v)
{
	cbt->iface.recno = cbt->recno = v;
}

/*
 * __cursor_position_clear --
 *	Forget the current key and value in a cursor.
 */
static inline void
__cursor_position_clear(WT_CURSOR_BTREE *cbt)
{
	F_CLR(&cbt->iface, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
}

/*
 * __cursor_search_clear --
 *	Reset the cursor's state for a search.
 */
static inline void
__cursor_search_clear(WT_CURSOR_BTREE *cbt)
{
	/* Our caller should have released any page held by this cursor. */
	cbt->page = NULL;
	cbt->slot = UINT32_MAX;			/* Fail big */

	cbt->ins_head = NULL;
	cbt->ins = NULL;
	cbt->ins_stack[0] = NULL;
	/* We don't bother clearing the insert stack, that's more expensive. */

	cbt->recno = 0;				/* Illegal value */
	cbt->write_gen = 0;

	cbt->compare = 2;			/* Illegal value */

	cbt->cip_saved = NULL;
	cbt->rip_saved = NULL;

	F_CLR(cbt, ~WT_CBT_ACTIVE);
}

/*
 * __cursor_leave --
 *	Clear a cursor's position.
 */
static inline int
__cursor_leave(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	/* The key and value may be gone, clear the flags here. */
	F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

	/* Release any page references we're holding. */
	WT_RET(__wt_page_release(session, cbt->page));
	cbt->page = NULL;

	if (F_ISSET(cbt, WT_CBT_ACTIVE)) {
		WT_ASSERT(session, session->ncursors > 0);
		if (--session->ncursors == 0)
			__wt_txn_read_last(session);
		F_CLR(cbt, WT_CBT_ACTIVE);

		/*
		 * If this is an autocommit operation that is just getting
		 * started, check that the cache isn't full.  We may have other
		 * cursors open, but the one we just closed might help eviction
		 * make progress.
		 */
		if (F_ISSET(&session->txn, TXN_AUTOCOMMIT))
			__wt_cache_full_check(session);
	}
	return (0);
}

/*
 * __cursor_enter --
 *	Setup the cursor's state for a new call.
 */
static inline void
__cursor_enter(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (session->ncursors++ == 0)
		__wt_txn_read_first(session);
	F_SET(cbt, WT_CBT_ACTIVE);
}

/*
 * __cursor_func_init --
 *	Cursor call setup.
 */
static inline int
__cursor_func_init(WT_CURSOR_BTREE *cbt, int reenter)
{
	if (reenter)
		WT_RET(__cursor_leave(cbt));
	if (!F_ISSET(cbt, WT_CBT_ACTIVE))
		__cursor_enter(cbt);
	return (0);
}

/*
 * __cursor_func_resolve --
 *	Resolve the cursor's state for return.
 */
static inline int
__cursor_func_resolve(WT_CURSOR_BTREE *cbt, int ret)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/*
	 * On success, we're returning a key/value pair, and can iterate.
	 * On error, we're not returning anything, we can't iterate, and
	 * we should release any page references we're holding.
	 */
	if (ret == 0) {
		F_CLR(cursor, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
	} else {
		WT_RET(__cursor_leave(cbt));
		__cursor_search_clear(cbt);
	}
	return (0);
}

/*
 * __cursor_row_slot_return --
 *	Return a WT_ROW slot's K/V pair.
 */
static inline int
__cursor_row_slot_return(WT_CURSOR_BTREE *cbt, WT_ROW *rip, WT_UPDATE *upd)
{
	WT_BTREE *btree;
	WT_ITEM *kb, *vb;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = session->btree;
	unpack = &_unpack;

	kb = &cbt->iface.key;
	vb = &cbt->iface.value;

	/*
	 * Return the WT_ROW slot's K/V pair.
	 */

	ikey = WT_ROW_KEY_COPY(rip);
	/*
	 * Key copied.
	 *
	 * If another thread instantiated the key, we don't have any work to do.
	 * Figure this out using the key's value:
	 *
	 * If the key points off-page, the key has been instantiated, just use
	 * it.
	 *
	 * If the key points on-page, we have a copy of a WT_CELL value that can
	 * be processed, regardless of what any other thread is doing.
	 */
	if (__wt_off_page(cbt->page, ikey)) {
		kb->data = WT_IKEY_DATA(ikey);
		kb->size = ikey->size;
	} else {
		/*
		 * Get a reference to the key, ideally without doing a copy.  If
		 * the key is simple and on-page, just reference it; if the key
		 * is simple, on-page and prefix-compressed, and we have the
		 * previous expanded key in the cursor buffer, build the key
		 * here.  Else, call the underlying routines to do it the hard
		 * way.
		 */
		if (btree->huffman_key != NULL)
			goto slow;
		__wt_cell_unpack((WT_CELL *)ikey, unpack);
		if (unpack->type == WT_CELL_KEY && unpack->prefix == 0) {
			kb->data = cbt->tmp.data = unpack->data;
			kb->size = cbt->tmp.size = unpack->size;
			cbt->rip_saved = rip;
		} else if (unpack->type == WT_CELL_KEY &&
		    cbt->rip_saved != NULL && cbt->rip_saved == rip - 1) {
			/*
			 * If we previously built a prefix-compressed key in the
			 * temporary buffer, the WT_ITEM->data field will be the
			 * same as the WT_ITEM->mem field: grow the buffer if
			 * necessary and copy the suffix into place.  If we
			 * previously pointed the temporary buffer at an on-page
			 * key, the WT_ITEM->data field will not be the same as
			 * the WT_ITEM->mem field: grow the buffer if necessary,
			 * copy the prefix into place, and then re-point the
			 * WT_ITEM->data field to the newly constructed memory.
			 */
			WT_RET(__wt_buf_grow(
			    session, &cbt->tmp, unpack->prefix + unpack->size));
			if (cbt->tmp.data != cbt->tmp.mem) {
				memcpy((uint8_t *)cbt->tmp.mem,
				    cbt->tmp.data, unpack->prefix);
				cbt->tmp.data = cbt->tmp.mem;
			}
			memcpy((uint8_t *)cbt->tmp.data +
			    unpack->prefix, unpack->data, unpack->size);
			cbt->tmp.size = unpack->prefix + unpack->size;
			kb->data = cbt->tmp.data;
			kb->size = cbt->tmp.size;
			cbt->rip_saved = rip;
		} else
slow:			WT_RET(__wt_row_key_copy(session, cbt->page, rip, kb));
	}

	/*
	 * If the item was ever modified, use the WT_UPDATE data.  Note that
	 * the caller passes us the update: it has already resolved which one
	 * (if any) is visible.
	 * Else, check for empty data.
	 * Else, use the value from the original disk image.
	 */
	if (upd != NULL) {
		vb->data = WT_UPDATE_DATA(upd);
		vb->size = upd->size;
	} else if ((cell = __wt_row_value(cbt->page, rip)) == NULL) {
		vb->data = "";
		vb->size = 0;
	} else {
		__wt_cell_unpack(cell, unpack);
		WT_RET(__wt_cell_unpack_ref(session, unpack, vb));
	}

	return (0);
}
