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

	/*
	 * If the cursor was active, decrement the count of active cursors in
	 * the session.  When that goes to zero, there are no active cursors,
	 * and we can release any snapshot we're holding for read committed
	 * isolation.
	 */
	if (F_ISSET(cbt, WT_CBT_ACTIVE)) {
		WT_ASSERT(session, session->ncursors > 0);
		if (--session->ncursors == 0)
			__wt_txn_read_last(session);
		F_CLR(cbt, WT_CBT_ACTIVE);
	}

	/*
	 * Release any page references we're holding.  This can trigger
	 * eviction (e.g., forced eviction of big pages), so it is important to
	 * do it after releasing our snapshot above.
	 */
	WT_RET(__wt_page_release(session, cbt->page));
	cbt->page = NULL;

	/*
	 * If this is an autocommit operation that is just getting started,
	 * check that the cache isn't full.  We may have other cursors open,
	 * but the one we just closed might help eviction make progress.
	 */
	if (F_ISSET(&session->txn, TXN_AUTOCOMMIT))
		WT_RET(__wt_cache_full_check(session, 1));

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
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (reenter)
		WT_RET(__cursor_leave(cbt));
	if (!F_ISSET(cbt, WT_CBT_ACTIVE))
		__cursor_enter(cbt);
	__wt_txn_cursor_op(session);
	return (0);
}

/*
 * __cursor_error_resolve --
 *	Resolve the cursor's state for return on error.
 */
static inline int
__cursor_error_resolve(WT_CURSOR_BTREE *cbt)
{
	/*
	 * On error, we can't iterate, so clear the cursor's position and
	 * release any page references we're holding.
	 */
	WT_RET(__cursor_leave(cbt));

	/* Clear the cursor's search state. */
	__cursor_search_clear(cbt);

	return (0);
}

/*
 * __cursor_row_slot_return --
 *	Return a row-store leaf page slot's K/V pair.
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
	btree = S2BT(session);
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
	 * Get a reference to the key, ideally without doing a copy: we could
	 * call __wt_row_leaf_key, but if a cursor is running through the tree,
	 * we actually have more information here than that function has, we
	 * may have the prefix-compressed key that comes immediately before the
	 * one we want.
	 *
	 * If the key has been instantiated (the key points off-page), we don't
	 * have any work to do.
	 *
	 * If the key points on-page, we have a copy of a WT_CELL value that can
	 * be processed, regardless of what any other thread is doing.
	 */
	if (__wt_off_page(cbt->page, ikey)) {
		kb->data = WT_IKEY_DATA(ikey);
		kb->size = ikey->size;
	} else {
		/*
		 * If the key is simple and on-page and not prefix-compressed,
		 * or we have the previous expanded key in the cursor buffer,
		 * reference or build it.  Else, call __wt_row_leaf_key_work to
		 * do it the hard way.
		 */
		if (btree->huffman_key != NULL)
			goto slow;
		__wt_cell_unpack((WT_CELL *)ikey, unpack);
		if (unpack->type == WT_CELL_KEY && unpack->prefix == 0) {
			cbt->tmp.data = unpack->data;
			cbt->tmp.size = unpack->size;
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
			WT_ASSERT(session, cbt->tmp.size >= unpack->prefix);
			if (cbt->tmp.data != cbt->tmp.mem)
				WT_RET(__wt_buf_set(session, &cbt->tmp,
				    cbt->tmp.data, unpack->prefix));
			WT_RET(__wt_buf_grow(
			    session, &cbt->tmp, unpack->prefix + unpack->size));
			memcpy((uint8_t *)cbt->tmp.data + unpack->prefix,
			    unpack->data, unpack->size);
			cbt->tmp.size = unpack->prefix + unpack->size;
		} else {
			/*
			 * __wt_row_leaf_key_work instead of __wt_row_leaf_key:
			 * we do __wt_row_leaf_key's fast-path checks inline.
			 */
slow:			WT_RET(__wt_row_leaf_key_work(
			    session, cbt->page, rip, &cbt->tmp, 0));
		}
		kb->data = cbt->tmp.data;
		kb->size = cbt->tmp.size;
		cbt->rip_saved = rip;
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
		WT_RET(__wt_cell_unpack_ref(
		    session, WT_PAGE_ROW_LEAF, unpack, vb));
	}

	return (0);
}
