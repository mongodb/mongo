/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __cursor_row_slot_return --
 *	Return a WT_ROW slot's K/V pair.
 */
static inline int
__cursor_row_slot_return(WT_CURSOR_BTREE *cbt, WT_ROW *rip)
{
	WT_BUF *key, *val;
	WT_CELL *cell;
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
	upd = WT_ROW_UPDATE(cbt->page, rip);
	if (upd != NULL) {
		val->data = WT_UPDATE_DATA(upd);
		val->size = upd->size;
	} else if ((cell = __wt_row_value(cbt->page, rip)) == NULL) {
		val->data = "";
		val->size = 0;
	} else
		WT_RET(__wt_cell_copy(session, cell, val));

	return (0);
}

/*
 * __cursor_search_reset --
 *	Reset the cursor's state for a search.
 */
static inline void
__cursor_search_reset(WT_CURSOR_BTREE *cbt)
{
	cbt->page = NULL;
	cbt->cip = NULL;
	cbt->rip = NULL;
	cbt->slot = UINT32_MAX;			/* Fail big. */

	cbt->ins_head = NULL;
	cbt->ins = NULL;

	cbt->match = 0;
	cbt->write_gen = 0;
}

