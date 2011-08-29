/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __cursor_fix_prev --
 *	Move to the previous, fixed-length column-store item.
 */
static inline int
__cursor_fix_prev(
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
		cbt->nslots = cbt->page->entries;
		cbt->recno =
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1);
	}

	/* This loop moves through a page, including after reading a record. */
	for (state = NOTFOUND; state != FOUND; --cbt->recno, --cbt->nslots) {
		if (cbt->nslots == 0)
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
 * __cursor_var_prev --
 *	Move to the previous, variable-length column-store item.
 */
static inline int
__cursor_var_prev(WT_CURSOR_BTREE *cbt,
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
		cbt->nslots = cbt->page->entries;
		cbt->recno =
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1);
		newcell = 1;
	} else
		newcell = 0;

	/* This loop moves through a page. */
	for (; cbt->rle_return_cnt > 0 || cbt->nslots > 0;
	    --cbt->cip, --cbt->nslots, newcell = 1) {
		/* Unpack each cell, find out how many times it's repeated. */
		if (newcell) {
			if ((cell = WT_COL_PTR(cbt->page, cbt->cip)) != NULL) {
				__wt_cell_unpack(cell, unpack);
				cbt->rle_return_cnt = unpack->rle;
			} else
				cbt->rle_return_cnt = 1;

			cbt->ins_head = WT_COL_INSERT(cbt->page, cbt->cip);

			/*
			 * Skip deleted records, there might be a large number
			 * of them.
			 */
			if (cbt->ins_head == NULL &&
			    unpack->type == WT_CELL_DEL) {
				cbt->recno -= cbt->rle_return_cnt;
				cbt->rle_return_cnt = 0;
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
		while (cbt->rle_return_cnt > 0) {
			--cbt->rle_return_cnt;
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
 * __cursor_row_prev --
 *	Move to the previous row-store item.
 */
static inline int
__cursor_row_prev(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BUF *key, *val;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	key = &cbt->iface.key;
	val = &cbt->iface.value;

	/*
	 * For row-store pages, we need a single item that tells us the part
	 * of the page we're walking (otherwise switching from next to prev
	 * and vice-versa is just too complicated), so we map the WT_ROW and
	 * WT_INSERT_HEAD array slots into a single name space: slot 1 is the
	 * "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is
	 * WT_INSERT_HEAD[0], and so on.  This means WT_INSERT lists are
	 * odd-numbered slots, and WT_ROW array slots are even-numbered slots.
	 *
	 * New page configuration.
	 */
	if (newpage) {
		cbt->ins_head =
		    WT_ROW_INSERT_SLOT(cbt->page, cbt->page->entries - 1);
		cbt->ins_entry_cnt = 0;
		WT_SKIP_FOREACH(ins, cbt->ins_head)
			++cbt->ins_entry_cnt;
		cbt->slot = cbt->page->entries * 2 + 1;
		goto new_insert;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		/*
		 * Continue traversing any insert list.  Insert lists are in
		 * forward sorted order; in a last-to-first walk we have walk
		 * the list from the end to the beginning.  Maintain the
		 * reference to the current insert element in case we switch
		 * to a cursor next movement.
		 */
		if (cbt->ins_head != NULL && cbt->ins_entry_cnt > 0)
			--cbt->ins_entry_cnt;

new_insert:	if (cbt->ins_head != NULL && cbt->ins_entry_cnt > 0) {
			for (i = cbt->ins_entry_cnt,
			    ins = WT_SKIP_FIRST(cbt->ins_head); i > 1; --i)
				ins = WT_SKIP_NEXT(ins);
			cbt->ins = ins;

			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd))
				continue;
			key->data = WT_INSERT_KEY(ins);
			key->size = WT_INSERT_KEY_SIZE(ins);
			val->data = WT_UPDATE_DATA(upd);
			val->size = upd->size;
			return (0);
		}

		/* Check for the beginning of the page. */
		if (cbt->slot == 1)
			return (WT_NOTFOUND);
		--cbt->slot;

		/*
		 * Odd-numbered slots configure as WT_INSERT_HEAD entries,
		 * even-numbered slots configure as WT_ROW entries.
		 */
		if (cbt->slot & 0x01) {
			cbt->ins_head = cbt->slot == 1 ?
			    WT_ROW_INSERT_SMALLEST(cbt->page) :
			    WT_ROW_INSERT_SLOT(cbt->page, cbt->slot / 2 - 1);
			cbt->ins_entry_cnt = 0;
			WT_SKIP_FOREACH(ins, cbt->ins_head)
				++cbt->ins_entry_cnt;
			goto new_insert;
		}
		cbt->ins_head = NULL;
		cbt->ins = NULL;

		rip = &cbt->page->u.row_leaf.d[cbt->slot / 2 - 1];
		upd = WT_ROW_UPDATE(cbt->page, rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		return (__cursor_row_slot_return(cbt, rip));
	}
	/* NOTREACHED */
}

/*
 * __wt_btcur_last --
 *	Move to the last record in the tree.
 */
int
__wt_btcur_last(WT_CURSOR_BTREE *cbt)
{
	__wt_cursor_clear(cbt);

	return (__wt_btcur_prev(cbt));
}

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int newpage, ret;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_readprev);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	/* If iterating from a search position, there's some setup to do. */
	if (F_ISSET(cbt, WT_CBT_SEARCH_SET))
		WT_RET(__wt_btcur_search_setup(cbt));

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the previous page, until we reach the start
	 * of the file.
	 */
	for (newpage = 0;; newpage = 1) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_prev(cbt, newpage,
				   &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_prev(cbt, newpage,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __cursor_row_prev(cbt, newpage);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}

		do {
			WT_ERR(__wt_tree_np(session, &cbt->page, 0));
			WT_ERR_TEST(cbt->page == NULL, WT_NOTFOUND);
		} while (
		    cbt->page->type == WT_PAGE_COL_INT ||
		    cbt->page->type == WT_PAGE_ROW_INT);
	}

err:	if (ret != 0)
		return (ret);

	F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	return (0);
}
