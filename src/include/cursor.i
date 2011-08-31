/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __cursor_search_reset --
 *	Reset the cursor's state for a search.
 */
static inline void
__cursor_search_reset(WT_CURSOR_BTREE *cbt)
{
	cbt->page = NULL;
	cbt->slot = UINT32_MAX;			/* Fail big. */

	cbt->ins_head = NULL;
	cbt->ins = NULL;
	/* We don't bother clearing the insert stack, that's more expensive. */

	cbt->match = 0;
	cbt->write_gen = 0;

	cbt->vslot = UINT32_MAX;
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
 * __cursor_col_rle_last --
 *	Return the last record number for a variable-length column-store page.
 */
static inline uint64_t
__cursor_col_rle_last(WT_PAGE *page)
{
	WT_COL_RLE *repeat;

	if (page->u.col_leaf.nrepeats == 0)
		return (page->u.col_leaf.recno + (page->entries - 1));

	repeat = &page->u.col_leaf.repeats[page->u.col_leaf.nrepeats - 1];
	return (
	    (repeat->recno + repeat->rle) - 1 +
	    (page->entries - (repeat->indx + 1)));
}

/*
 * __cursor_col_rle_search --
 *	Search a variable-length column-store page for a record.
 */
static inline WT_COL *
__cursor_col_rle_search(WT_PAGE *page, uint64_t recno)
{
	WT_COL_RLE *repeat;
	uint64_t start_recno;
	uint32_t base, indx, limit, start_indx;

	/*
	 * Find the matching slot.
	 *
	 * This is done in two stages: first, we do a binary search among any
	 * repeating records to find largest repeating less than the search key.
	 * Once there, we can do a simple offset calculation to find the correct
	 * slot for this record number, because we know any intervening records
	 * have repeat counts of 1.
	 */
	for (base = 0,
	    limit = page->u.col_leaf.nrepeats; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);

		repeat = page->u.col_leaf.repeats + indx;
		if (recno >= repeat->recno &&
		    recno < repeat->recno + repeat->rle)
			return (page->u.col_leaf.d + repeat->indx);
		if (recno < repeat->recno)
			continue;
		base = indx + 1;
		--limit;
	}

	/*
	 * We didn't find an exact match, move forward from the largest repeat
	 * less than the search key.
	 */
	if (base == 0) {
		start_indx = 0;
		start_recno = page->u.col_leaf.recno;
	} else {
		repeat = page->u.col_leaf.repeats + (base - 1);
		start_indx = repeat->indx + 1;
		start_recno = repeat->recno + repeat->rle;
	}

	if (recno >= start_recno + (page->entries - start_indx))
		return (NULL);

	return (page->u.col_leaf.d +
	    start_indx + (uint32_t)(recno - start_recno));
}
