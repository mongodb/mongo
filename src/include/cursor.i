/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	cbt->ref = NULL;

	/*
	 * Set the on-page slot to an impossible value larger than any possible
	 * slot (it's used to validate the search function's return).
	 */
	cbt->slot = UINT32_MAX;

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
 * __cursor_enter --
 *	Activate a cursor.
 */
static inline int
__cursor_enter(WT_SESSION_IMPL *session)
{
	/*
	 * If there are no other cursors positioned in the session, check
	 * whether the cache is full and then get a snapshot if necessary.
	 */
	if (session->ncursors == 0) {
		WT_RET(__wt_cache_full_check(session));
		__wt_txn_read_first(session);
	}
	++session->ncursors;
	return (0);
}

/*
 * __cursor_leave --
 *	Deactivate a cursor.
 */
static inline int
__cursor_leave(WT_SESSION_IMPL *session)
{
	/*
	 * Decrement the count of active cursors in the session.  When that
	 * goes to zero, there are no active cursors, and we can release any
	 * snapshot we're holding for read committed isolation.
	 */
	WT_ASSERT(session, session->ncursors > 0);
	if (--session->ncursors == 0)
		__wt_txn_read_last(session);

	return (0);
}

/*
 * __curfile_enter --
 *	Activate a file cursor.
 */
static inline int
__curfile_enter(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_RET(__cursor_enter(session));
	F_SET(cbt, WT_CBT_ACTIVE);
	return (0);
}

/*
 * __curfile_leave --
 *	Clear a file cursor's position.
 */
static inline int
__curfile_leave(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/* If the cursor was active, deactivate it. */
	if (F_ISSET(cbt, WT_CBT_ACTIVE)) {
		WT_RET(__cursor_leave(session));
		F_CLR(cbt, WT_CBT_ACTIVE);
	}

	/*
	 * Release any page references we're holding.  This can trigger
	 * eviction (e.g., forced eviction of big pages), so it is important to
	 * do it after releasing our snapshot above.
	 */
	WT_RET(__wt_page_release(session, cbt->ref));
	cbt->ref = NULL;

	return (0);
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
		WT_RET(__curfile_leave(cbt));
	if (!F_ISSET(cbt, WT_CBT_ACTIVE))
		WT_RET(__curfile_enter(cbt));
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
	WT_RET(__curfile_leave(cbt));

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
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	int key_unpacked;
	void *copy;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = S2BT(session);
	page = cbt->ref->page;

	unpack = &_unpack;
	key_unpacked = 0;

	kb = &cbt->iface.key;
	vb = &cbt->iface.value;

	/*
	 * The row-store key can change underfoot; explicitly take a copy.
	 */
	copy = WT_ROW_KEY_COPY(rip);

	/*
	 * Get a key: we could just call __wt_row_leaf_key, but as a cursor
	 * is running through the tree, we may have additional information
	 * here (we may have the fully-built key that's immediately before
	 * the prefix-compressed key one we want).
	 *
	 * First, check for an immediately available key.
	 */
	if (__wt_row_leaf_key_info(
	    page, copy, NULL, &cell, &kb->data, &kb->size))
		goto value;

	/* Huffman encoded keys are a slow path in all cases. */
	if (btree->huffman_key != NULL)
		goto slow;

	/*
	 * Unpack the cell and deal with overflow and prefix-compressed keys.
	 * Inline building simple prefix-compressed keys from a previous key,
	 * otherwise build from scratch.
	 */
	__wt_cell_unpack(cell, unpack);
	key_unpacked = 1;
	if (unpack->type == WT_CELL_KEY &&
	    cbt->rip_saved != NULL && cbt->rip_saved == rip - 1) {
		WT_ASSERT(session, cbt->tmp.size >= unpack->prefix);

		/*
		 * Grow the buffer as necessary as well as ensure data has been
		 * copied into local buffer space, then append the suffix to the
		 * prefix already in the buffer.
		 *
		 * Don't grow the buffer unnecessarily or copy data we don't
		 * need, truncate the item's data length to the prefix bytes.
		 */
		cbt->tmp.size = unpack->prefix;
		WT_RET(__wt_buf_grow(
		    session, &cbt->tmp, cbt->tmp.size + unpack->size));
		memcpy((uint8_t *)cbt->tmp.data + cbt->tmp.size,
		    unpack->data, unpack->size);
		cbt->tmp.size += unpack->size;
	} else {
		/*
		 * Call __wt_row_leaf_key_work instead of __wt_row_leaf_key: we
		 * already did __wt_row_leaf_key's fast-path checks inline.
		 */
slow:		WT_RET(
		    __wt_row_leaf_key_work(session, page, rip, &cbt->tmp, 0));
	}
	kb->data = cbt->tmp.data;
	kb->size = cbt->tmp.size;
	cbt->rip_saved = rip;

value:
	/*
	 * If the item was ever modified, use the WT_UPDATE data.  Note the
	 * caller passes us the update: it has already resolved which one
	 * (if any) is visible.
	 */
	if (upd != NULL) {
		vb->data = WT_UPDATE_DATA(upd);
		vb->size = upd->size;
		return (0);
	}

	/*
	 * Else, find the value cell and check for empty data.
	 * Else, use the value from the original disk image.
	 */
	cell = __wt_row_leaf_value(page, rip, key_unpacked ? unpack : NULL);
	if (cell == NULL) {
		vb->data = "";
		vb->size = 0;
	} else {
		__wt_cell_unpack(cell, unpack);
		WT_RET(__wt_page_cell_data_ref(
		    session, cbt->ref->page, unpack, vb));
	}

	return (0);
}
