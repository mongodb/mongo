/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __btcur_prev_fix --
 *	Move to the previous, fixed-length column-store item.
 */
static inline int
__btcur_prev_fix(
    WT_CURSOR_BTREE *cbt, int newpage, uint64_t *recnop, WT_BUF *value)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	enum { DELETED, FOUND, NOTFOUND } state;
	uint8_t v;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = session->btree;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->ins_head = WT_COL_INSERT_SINGLE(cbt->page);
		cbt->nitems = cbt->page->entries;
		cbt->recno =
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1);
	}

	/* This loop moves through a page, including after reading a record. */
	for (state = NOTFOUND; state != FOUND; --cbt->recno, --cbt->nitems) {
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		*recnop = cbt->recno;

		/*
		 * Check any insert list for a matching record.  Insert lists
		 * are in forward sorted order; in a last-to-first walk we have
		 * to search the entire list.
		 */
		state = NOTFOUND;
		if (cbt->ins_head != NULL)
			WT_SKIP_FOREACH(ins, cbt->ins_head) {
				if (cbt->recno != WT_INSERT_RECNO(ins))
					continue;
				upd = cbt->ins->upd;
				if (WT_UPDATE_DELETED_ISSET(upd))
					state = DELETED;
				else {
					value->data =
					    WT_UPDATE_DATA(upd);
					value->size = 1;
					state = FOUND;
				}
			}
		if (state == NOTFOUND) {
			v = __bit_getv_recno(
			    cbt->page, cbt->recno, btree->bitcnt);
			WT_RET(__wt_buf_set(session, &cbt->value, &v, 1));
			value->data = cbt->value.data;
			value->size = 1;
			state = FOUND;
		}
	}

	return (0);
}

/*
 * __btcur_prev_var --
 *	Move to the previous, variable-length column-store item.
 */
static inline int
__btcur_prev_var(WT_CURSOR_BTREE *cbt,
    int newpage, uint64_t *recnop, WT_BUF *value)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	enum { DELETED, FOUND, NOTFOUND } state;
	int newcell;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	unpack = &_unpack;
	cell = NULL;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->cip = cbt->page->u.col_leaf.d + (cbt->page->entries - 1);
		cbt->nitems = cbt->page->entries;
		cbt->recno =
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1);
		newcell = 1;
	} else
		newcell = 0;

	/* This loop moves through a page. */
	for (; cbt->rle > 0 || cbt->nitems > 0;
	    --cbt->cip, --cbt->nitems, newcell = 1) {
		/* Unpack each cell, find out how many times it's repeated. */
		if (newcell) {
			if ((cell = WT_COL_PTR(cbt->page, cbt->cip)) != NULL) {
				__wt_cell_unpack(cell, unpack);
				cbt->rle = unpack->rle;
			} else
				cbt->rle = 1;

			cbt->ins_head = WT_COL_INSERT(cbt->page, cbt->cip);

			/*
			 * Skip deleted records, there might be a large number
			 * of them.
			 */
			if (cbt->ins_head == NULL &&
			    unpack->type == WT_CELL_DEL) {
				cbt->recno -= cbt->rle;
				cbt->rle = 0;
				continue;
			}

			/*
			 * Get a copy of the item we're returning: it might be
			 * encoded, and we don't want to repeatedly decode it.
			 */
			if (cell == NULL) {
				cbt->value.data = NULL;
				cbt->value.size = 0;
			} else
				WT_RET(__wt_cell_unpack_copy(
				    session, unpack, &cbt->value));
		}

		/* Return the data RLE-count number of times. */
		state = NOTFOUND;
		while (cbt->rle > 0) {
			--cbt->rle;
			*recnop = cbt->recno--;

			/*
			 * Check any insert list for a matching record.  Insert
			 * lists are in forward sorted order; in a last-to-first
			 * walk we have to search the entire list.
			 */
			if (cbt->ins_head != NULL)
				WT_SKIP_FOREACH(ins, cbt->ins_head) {
					if (cbt->recno !=
					    WT_INSERT_RECNO(cbt->ins))
						continue;
					upd = cbt->ins->upd;
					if (WT_UPDATE_DELETED_ISSET(upd))
						state = DELETED;
					else {
						value->data =
						    WT_UPDATE_DATA(upd);
						value->size = upd->size;
						state = FOUND;
					}
					break;
				}
			if (state == NOTFOUND) {
				value->data = cbt->value.data;
				value->size = cbt->value.size;
				state = FOUND;
			}
			if (state == FOUND)
				return (0);
		}
	}
	return (WT_NOTFOUND);
}

/*
 * __btcur_prev_row --
 *	Move to the previous row-store item.
 */
static inline int
__btcur_prev_row(WT_CURSOR_BTREE *cbt, int newpage, WT_BUF *key, WT_BUF *value)
{
	WT_CELL *cell;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint32_t i;
	int found;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->rip = cbt->page->u.row_leaf.d + (cbt->page->entries - 1);
		cbt->nitems = cbt->page->entries;
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(cbt->page);
	}

	/* This loop moves through a page, including after reading a record. */
	for (found = 0; !found;
	    cbt->ins_head = WT_ROW_INSERT(cbt->page, cbt->rip),
	    --cbt->rip, --cbt->nitems) {
		/*
		 * Continue traversing any insert list.  Insert lists are in
		 * forward sorted order; in a last-to-first walk we have walk
		 * the list from the end to the beginning.
		 */
		if (cbt->ins_head != NULL) {
			/* Count the number of items on the insert list. */
			if (cbt->ins_cnt == 0)
				WT_SKIP_FOREACH(ins, cbt->ins_head)
					++cbt->ins_cnt;

			/* Return them in reverse order. */
			for (i = cbt->ins_cnt,
			    ins = WT_SKIP_FIRST(cbt->ins_head); i > 0; --i)
				ins = WT_SKIP_NEXT(ins);
			if (--cbt->ins_cnt == 0)
				cbt->ins_head = NULL;

			upd = ins->upd;
			if (!WT_UPDATE_DELETED_ISSET(upd)) {
				key->data = WT_INSERT_KEY(ins);
				key->size = WT_INSERT_KEY_SIZE(ins);
				value->data = WT_UPDATE_DATA(upd);
				value->size = upd->size;
				return (0);
			}
		}

		/* Check to see if we've completed the page. */
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		/* If the slot has been deleted, we don't have a record. */
		upd = WT_ROW_UPDATE(cbt->page, cbt->rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		/* We have a record. */
		found = 1;

		/*
		 * Set the key.
		 *
		 * XXX
		 * If we have the last key, we can easily build the next prefix
		 * compressed key without calling __wt_row_key() -- obviously,
		 * that won't work for overflow or Huffman-encoded keys, so we
		 * need to check the cell type, at the least, before taking the
		 * fast path.
		 */
		if (__wt_off_page(cbt->page, cbt->rip->key)) {
			ikey = cbt->rip->key;
			key->data = WT_IKEY_DATA(ikey);
			key->size = ikey->size;
		} else
			WT_RET(__wt_row_key(session, cbt->page, cbt->rip, key));

		/*
		 * If the item was ever modified, use the data from the
		 * WT_UPDATE entry. Then check for empty data.  Finally, use
		 * the value from the disk image.
		 */
		if (upd != NULL) {
			value->data = WT_UPDATE_DATA(upd);
			value->size = upd->size;
		} else if ((cell =
		    __wt_row_value(cbt->page, cbt->rip)) == NULL) {
			value->data = "";
			value->size = 0;
		} else
			WT_RET(__wt_cell_copy(session, cell, value));
	}

	return (0);
}

/*
 * __wt_btcur_last --
 *	Move to the last record in the tree.
 */
int
__wt_btcur_last(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_RET(__wt_walk_last(session, NULL, &cbt->walk, 0));
	return (__wt_btcur_prev(cbt));
}

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	int newpage, ret;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_BSTAT_INCR(session, file_reads);

	if (cbt->walk.tree == NULL)
		return (__wt_btcur_last(cbt));

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the previous page, until we reach the start
	 * of the file.
	 */
	for (newpage = 0;; newpage = 1) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __btcur_prev_fix(cbt, newpage,
				   &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_COL_VAR:
				ret = __btcur_prev_var(cbt, newpage,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __btcur_prev_row(cbt, newpage,
				    &cursor->key, &cursor->value);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}

		do {
			WT_ERR(__wt_walk_prev(session, &cbt->walk, &cbt->page));
			WT_ERR_TEST(cbt->page == NULL, WT_NOTFOUND);
		} while (
		    cbt->page->type == WT_PAGE_COL_INT ||
		    cbt->page->type == WT_PAGE_ROW_INT);
	}

err:	if (ret == 0)
		F_SET(cursor, WT_CURSTD_POSITIONED |
		    WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		F_CLR(cursor, WT_CURSTD_POSITIONED |
		    WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	return (ret);
}
