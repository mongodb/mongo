/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
 * __cursor_pos_clear --
 *	Reset the cursor's location.
 */
static inline void
__cursor_pos_clear(WT_CURSOR_BTREE *cbt)
{
	/*
	 * Most of the cursor's location information that needs to be set on
	 * successful return is always set by a successful return, for example,
	 * we don't initialize the compare return value because it's always
	 * set by the row-store search.  The other stuff gets cleared here,
	 * and it's a minimal set of things we need to clear. It would be a
	 * lot simpler to clear everything, but we call this function a lot.
	 */
	cbt->recno = 0;

	cbt->ins = NULL;
	cbt->ins_head = NULL;
	cbt->ins_stack[0] = NULL;

	cbt->cip_saved = NULL;
	cbt->rip_saved = NULL;

	/*
	 * Don't clear the active flag, it's owned by the cursor enter/leave
	 * functions.
	 */
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
	 * whether the cache is full.
	 */
	if (session->ncursors == 0)
		WT_RET(__wt_cache_full_check(session));
	++session->ncursors;
	return (0);
}

/*
 * __cursor_leave --
 *	Deactivate a cursor.
 */
static inline void
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
		__cursor_leave(session);
		F_CLR(cbt, WT_CBT_ACTIVE);
	}

	/*
	 * If we were scanning and saw a lot of deleted records on this page,
	 * try to evict the page when we release it.
	 */
	if (cbt->ref != NULL &&
	    cbt->page_deleted_count > WT_BTREE_DELETE_THRESHOLD)
		__wt_page_evict_soon(cbt->ref->page);
	cbt->page_deleted_count = 0;

	/*
	 * Release any page references we're holding.  This can trigger
	 * eviction (e.g., forced eviction of big pages), so it is important to
	 * do it after releasing our snapshot above.
	 */
	WT_RET(__wt_page_release(session, cbt->ref, 0));
	cbt->ref = NULL;
	return (0);
}

/*
 * __wt_cursor_dhandle_incr_use --
 *	Increment the in-use counter in cursor's data source.
 */
static inline void
__wt_cursor_dhandle_incr_use(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;

	/* If we open a handle with a time of death set, clear it. */
	if (WT_ATOMIC_ADD4(dhandle->session_inuse, 1) == 1 &&
	    dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*
 * __wt_cursor_dhandle_decr_use --
 *	Decrement the in-use counter in cursor's data source.
 */
static inline void
__wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;

	/* If we close a handle with a time of death set, clear it. */
	WT_ASSERT(session, dhandle->session_inuse > 0);
	if (WT_ATOMIC_SUB4(dhandle->session_inuse, 1) == 0 &&
	    dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*
 * __cursor_func_init --
 *	Cursor call setup.
 */
static inline int
__cursor_func_init(WT_CURSOR_BTREE *cbt, int reenter)
{
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	txn = &session->txn;

	if (reenter)
		WT_RET(__curfile_leave(cbt));

	/*
	 * If there is no transaction active in this thread and we haven't
	 * checked if the cache is full, do it now.  If we have to block for
	 * eviction, this is the best time to do it.
	 */
	if (F_ISSET(txn, TXN_RUNNING) &&
	    !F_ISSET(txn, TXN_HAS_ID) && !F_ISSET(txn, TXN_HAS_SNAPSHOT))
		WT_RET(__wt_cache_full_check(session));

	if (!F_ISSET(cbt, WT_CBT_ACTIVE))
		WT_RET(__curfile_enter(cbt));
	__wt_txn_cursor_op(session);
	return (0);
}

/*
 * __cursor_reset --
 *	Reset the cursor.
 */
static inline int
__cursor_reset(WT_CURSOR_BTREE *cbt)
{
	WT_DECL_RET;

	/*
	 * The cursor is leaving the API, and no longer holds any position,
	 * generally called to clean up the cursor after an error.
	 */
	ret = __curfile_leave(cbt);
	__cursor_pos_clear(cbt);
	return (ret);
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
	void *copy;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = S2BT(session);
	page = cbt->ref->page;

	unpack = NULL;

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
	 * the prefix-compressed key we want, so it's a faster construction).
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
	unpack = &_unpack;
	__wt_cell_unpack(cell, unpack);
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

	/* Else, simple values have their location encoded in the WT_ROW. */
	if (__wt_row_leaf_value(page, rip, vb))
		return (0);

	/*
	 * Else, take the value from the original page cell (which may be
	 * empty).
	 */
	if ((cell = __wt_row_leaf_value_cell(page, rip, unpack)) == NULL) {
		vb->data = "";
		vb->size = 0;
		return (0);
	}

	unpack = &_unpack;
	__wt_cell_unpack(cell, unpack);
	return (__wt_page_cell_data_ref(session, cbt->ref->page, unpack, vb));
}
