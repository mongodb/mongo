/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
	cbt->recno = WT_RECNO_OOB;

	cbt->ins = NULL;
	cbt->ins_head = NULL;
	cbt->ins_stack[0] = NULL;

	cbt->cip_saved = NULL;
	cbt->rip_saved = NULL;

	F_CLR(cbt, WT_CBT_POSITION_MASK);
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
		WT_RET(__wt_cache_eviction_check(session, false, NULL));
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

	if (!F_ISSET(cbt, WT_CBT_NO_TXN))
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/* If the cursor was active, deactivate it. */
	if (F_ISSET(cbt, WT_CBT_ACTIVE)) {
		if (!F_ISSET(cbt, WT_CBT_NO_TXN))
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
	 * Release any page references we're holding. This can trigger eviction
	 * (e.g., forced eviction of big pages), so it's important to do after
	 * releasing our snapshot above.
	 *
	 * Clear the reference regardless, so we don't try the release twice.
	 */
	ret = __wt_page_release(session, cbt->ref, 0);
	cbt->ref = NULL;

	return (ret);
}

/*
 * __wt_curindex_get_valuev --
 *	Internal implementation of WT_CURSOR->get_value for index cursors
 */
static inline int
__wt_curindex_get_valuev(WT_CURSOR *cursor, va_list ap)
{
	WT_CURSOR_INDEX *cindex;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;

	cindex = (WT_CURSOR_INDEX *)cursor;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_CURSOR_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
		ret = __wt_schema_project_merge(session,
		    cindex->cg_cursors, cindex->value_plan,
		    cursor->value_format, &cursor->value);
		if (ret == 0) {
			item = va_arg(ap, WT_ITEM *);
			item->data = cursor->value.data;
			item->size = cursor->value.size;
		}
	} else
		ret = __wt_schema_project_out(session,
		    cindex->cg_cursors, cindex->value_plan, ap);
err:	return (ret);
}

/*
 * __wt_curtable_get_valuev --
 *	Internal implementation of WT_CURSOR->get_value for table cursors.
 */
static inline int
__wt_curtable_get_valuev(WT_CURSOR *cursor, va_list ap)
{
	WT_CURSOR *primary;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	session = (WT_SESSION_IMPL *)cursor->session;
	primary = *ctable->cg_cursors;
	WT_CURSOR_NEEDVALUE(primary);

	if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
		ret = __wt_schema_project_merge(session,
		    ctable->cg_cursors, ctable->plan,
		    cursor->value_format, &cursor->value);
		if (ret == 0) {
			item = va_arg(ap, WT_ITEM *);
			item->data = cursor->value.data;
			item->size = cursor->value.size;
		}
	} else
		ret = __wt_schema_project_out(session,
		    ctable->cg_cursors, ctable->plan, ap);
err:	return (ret);
}

/*
 * __wt_cursor_dhandle_incr_use --
 *	Increment the in-use counter in the cursor's data source.
 */
static inline void
__wt_cursor_dhandle_incr_use(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;

	/* If we open a handle with a time of death set, clear it. */
	if (__wt_atomic_addi32(&dhandle->session_inuse, 1) == 1 &&
	    dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*
 * __wt_cursor_dhandle_decr_use --
 *	Decrement the in-use counter in the cursor's data source.
 */
static inline void
__wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;

	/* If we close a handle with a time of death set, clear it. */
	WT_ASSERT(session, dhandle->session_inuse > 0);
	if (__wt_atomic_subi32(&dhandle->session_inuse, 1) == 0 &&
	    dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*
 * __cursor_func_init --
 *	Cursor call setup.
 */
static inline int
__cursor_func_init(WT_CURSOR_BTREE *cbt, bool reenter)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (reenter) {
#ifdef HAVE_DIAGNOSTIC
		__wt_cursor_key_order_reset(cbt);
#endif
		WT_RET(__curfile_leave(cbt));
	}

	/*
	 * Any old insert position is now invalid.  We rely on this being
	 * cleared to detect if a new skiplist is installed after a search.
	 */
	cbt->ins_stack[0] = NULL;

	/* If the transaction is idle, check that the cache isn't full. */
	WT_RET(__wt_txn_idle_cache_check(session));

	if (!F_ISSET(cbt, WT_CBT_ACTIVE))
		WT_RET(__curfile_enter(cbt));

	/*
	 * If this is an ordinary transactional cursor, make sure we are set up
	 * to read.
	 */
	if (!F_ISSET(cbt, WT_CBT_NO_TXN))
		WT_RET(__wt_txn_cursor_op(session));
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
		WT_ASSERT(session, cbt->row_key->size >= unpack->prefix);

		/*
		 * Grow the buffer as necessary as well as ensure data has been
		 * copied into local buffer space, then append the suffix to the
		 * prefix already in the buffer.
		 *
		 * Don't grow the buffer unnecessarily or copy data we don't
		 * need, truncate the item's data length to the prefix bytes.
		 */
		cbt->row_key->size = unpack->prefix;
		WT_RET(__wt_buf_grow(
		    session, cbt->row_key, cbt->row_key->size + unpack->size));
		memcpy((uint8_t *)cbt->row_key->data + cbt->row_key->size,
		    unpack->data, unpack->size);
		cbt->row_key->size += unpack->size;
	} else {
		/*
		 * Call __wt_row_leaf_key_work instead of __wt_row_leaf_key: we
		 * already did __wt_row_leaf_key's fast-path checks inline.
		 */
slow:		WT_RET(__wt_row_leaf_key_work(
		    session, page, rip, cbt->row_key, false));
	}
	kb->data = cbt->row_key->data;
	kb->size = cbt->row_key->size;
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
