/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

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
	/* We don't bother clearing the insert stack, that's more expensive. */

	cbt->recno = 0;				/* Illegal value */
	cbt->write_gen = 0;

	cbt->compare = 2;			/* Illegal value */

	cbt->cip_saved = NULL;

	cbt->flags = 0;
}

/*
 * __cursor_func_init --
 *	Reset the cursor's state for a new call.
 */
static inline void
__cursor_func_init(WT_CURSOR_BTREE *cbt, int page_release)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	/* Optionally release any page references we're holding. */
	if (page_release && cbt->page != NULL) {
		__wt_page_release(session, cbt->page);
		cbt->page = NULL;
	}

	/* Reset the returned key/value state. */
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
}

/*
 * __cursor_func_resolve --
 *	Resolve the cursor's state for return.
 */
static inline void
__cursor_func_resolve(WT_CURSOR_BTREE *cbt, int ret)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	/*
	 * On success, we're returning a key/value pair, and can iterate.
	 * On error, we're not returning anything, we can't iterate, and
	 * we should release any page references we're holding.
	 */
	if (ret == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else {
		__cursor_func_init(cbt, 1);
		__cursor_search_clear(cbt);
	}
}

/*
 * __cursor_row_slot_return --
 *	Return a WT_ROW slot's K/V pair.
 */
static inline int
__cursor_row_slot_return(WT_CURSOR_BTREE *cbt, WT_ROW *rip)
{
	WT_BUF *key, *val;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_IKEY *ikey;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	key = &cbt->iface.key;
	val = &cbt->iface.value;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/*
	 * Return the WT_ROW slot's K/V pair.
	 *
	 * XXX
	 * If we have the last key, we can easily build the next prefix
	 * compressed key without calling __wt_row_key() -- obviously,
	 * that won't work for overflow or Huffman-encoded keys, so we
	 * need to check the cell type, at the least, before taking the
	 * fast path.
	 */
	if (__wt_off_page(cbt->page, rip->key)) {
		ikey = rip->key;
		key->data = WT_IKEY_DATA(ikey);
		key->size = ikey->size;
	} else
		WT_RET(__wt_row_key(session, cbt->page, rip, key));

	/*
	 * If the item was ever modified, use the WT_UPDATE data.
	 * Else, check for empty data.
	 * Else, use the value from the original disk image.
	 */
	if ((upd = WT_ROW_UPDATE(cbt->page, rip)) != NULL) {
		val->data = WT_UPDATE_DATA(upd);
		val->size = upd->size;
	} else if ((cell = __wt_row_value(cbt->page, rip)) == NULL) {
		val->data = "";
		val->size = 0;
	} else {
		__wt_cell_unpack(cell, &unpack);
		if (unpack.type == WT_CELL_VALUE &&
		    session->btree->huffman_value == NULL) {
			val->data = unpack.data;
			val->size = unpack.size;
		} else
			WT_RET(__wt_cell_unpack_copy(session, &unpack, val));
	}

	return (0);
}
