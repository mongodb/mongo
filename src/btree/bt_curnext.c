/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __cursor_fix_next --
 *	Move to the next, fixed-length column-store item.
 */
static inline int
__cursor_fix_next(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BTREE *btree;
	WT_BUF *val;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint64_t *recnop;
	uint8_t v;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = session->btree;

	recnop = &cbt->iface.recno;
	val = &cbt->iface.value;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->ins = WT_SKIP_FIRST(WT_COL_INSERT_SINGLE(cbt->page));
		cbt->recno = cbt->page->u.col_leaf.recno;
		goto new_page;
	}

	/*
	 * The cursor prev function may have cleared our insert list reference,
	 * so we have to check in on every call.
	 */
	if (cbt->ins == NULL)
		cbt->ins = WT_SKIP_FIRST(WT_COL_INSERT_SINGLE(cbt->page));

	/* Move to the next entry and return the item. */
	for (;;) {
		if (cbt->recno >=
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1))
			return (WT_NOTFOUND);
		++cbt->recno;
new_page:	*recnop = cbt->recno;

		/*
		 * Check any insert list for a matching record.  Insert lists
		 * are in forward sorted order, move forward until we reach a
		 * value equal to or larger than the target record number, then
		 * check for equality.
		 *
		 * If doing a next after a previous cursor next, the insert list
		 * reference should be pointing to the record we want; if we're
		 * doing a next after a search, the insert list reference may be
		 * pointing to the beginning of the list; if we're doing a next
		 * after a previous cursor prev, the insert list reference will
		 * be the beginning of the list (the cursor prev code clears the
		 * insert list reference, and we set it to the beginning of the
		 * list at the start of this function).   In all cases we can
		 * move forward to the entry we want, and leave the insert list
		 * reference set for a future cursor-next operation.
		 */
		while (cbt->ins != NULL &&
		    WT_INSERT_RECNO(cbt->ins) < cbt->recno)
			cbt->ins = WT_SKIP_NEXT(cbt->ins);
		if (cbt->ins != NULL &&
		    WT_INSERT_RECNO(cbt->ins) == cbt->recno) {
			upd = cbt->ins->upd;
			cbt->ins = WT_SKIP_NEXT(cbt->ins);
			val->data = WT_UPDATE_DATA(upd);
			val->size = 1;
			return (0);
		}

		v = __bit_getv_recno(cbt->page, cbt->recno, btree->bitcnt);
		WT_RET(__wt_buf_set(session, &cbt->value, &v, 1));
		val->data = cbt->value.data;
		val->size = 1;
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __cursor_var_next --
 *	Move to the next, variable-length column-store item.
 */
static inline int
__cursor_var_next(WT_CURSOR_BTREE *cbt,
    int newpage, uint64_t *recnop, WT_BUF *value)
{
	WT_SESSION_IMPL *session;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_UPDATE *upd;
	int newcell;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	unpack = &_unpack;
	cell = NULL;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nslots = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
		newcell = 1;
	} else
		newcell = 0;

	/* This loop moves through a page. */
	for (; cbt->rle_return_cnt > 0 || cbt->nslots > 0;
	    ++cbt->cip, --cbt->nslots, newcell = 1) {
		/* Unpack each cell, find out how many times it's repeated. */
		if (newcell) {
			if ((cell = WT_COL_PTR(cbt->page, cbt->cip)) != NULL) {
				__wt_cell_unpack(cell, unpack);
				cbt->rle_return_cnt = unpack->rle;
			} else
				cbt->rle_return_cnt = 1;

			cbt->ins = WT_SKIP_FIRST(
			    WT_COL_INSERT(cbt->page, cbt->cip));

			/*
			 * Skip deleted records, there might be a large number
			 * of them.
			 */
			if (cbt->ins == NULL && unpack->type == WT_CELL_DEL) {
				cbt->recno += cbt->rle_return_cnt;
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
		while (cbt->rle_return_cnt > 0) {
			--cbt->rle_return_cnt;
			*recnop = cbt->recno++;

			/*
			 * Check any insert list for a matching record (insert
			 * lists are in sorted order, we check the next entry).
			 */
			if (cbt->ins != NULL &&
			    WT_INSERT_RECNO(cbt->ins) == cbt->recno) {
				upd = cbt->ins->upd;
				cbt->ins = WT_SKIP_NEXT(cbt->ins);

				if (WT_UPDATE_DELETED_ISSET(upd))
					continue;
				value->data = WT_UPDATE_DATA(upd);
				value->size = upd->size;
			} else {
				value->data = cbt->value.data;
				value->size = cbt->value.size;
			}
			return (0);
		}
	}
	return (WT_NOTFOUND);
}

/*
 * __cursor_row_next --
 *	Move to the next row-store item.
 */
static inline int
__cursor_row_next(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BUF *key, *val;
	WT_ROW *rip;
	WT_UPDATE *upd;

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
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(cbt->page);
		cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
		cbt->ins_entry_cnt = 1;
		cbt->slot = 1;
		goto new_insert;
	}

	/* Move to the next entry and return the item. */
	for (;;) {
		/*
		 * Continue traversing any insert list; maintain the insert list
		 * head reference and entry count in case we switch to a cursor
		 * previous movement.
		 */
		if (cbt->ins != NULL) {
			++cbt->ins_entry_cnt;
			cbt->ins = WT_SKIP_NEXT(cbt->ins);
		}

new_insert:	if (cbt->ins != NULL) {
			upd = cbt->ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd))
				continue;
			key->data = WT_INSERT_KEY(cbt->ins);
			key->size = WT_INSERT_KEY_SIZE(cbt->ins);
			val->data = WT_UPDATE_DATA(upd);
			val->size = upd->size;
			return (0);
		}

		/* Check for the end of the page. */
		if (cbt->slot == cbt->page->entries * 2 + 1)
			return (WT_NOTFOUND);
		++cbt->slot;

		/*
		 * Odd-numbered slots configure as WT_INSERT_HEAD entries,
		 * even-numbered slots configure as WT_ROW entries.
		 */
		if (cbt->slot & 0x01) {
			cbt->ins_head =
			    WT_ROW_INSERT_SLOT(cbt->page, cbt->slot / 2 - 1);
			cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
			cbt->ins_entry_cnt = 1;
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
 * __wt_btcur_search_setup --
 *	Initialize a cursor for iteration based on a search.
 */
int
__wt_btcur_search_setup(WT_CURSOR_BTREE *cbt)
{
	WT_INSERT *ins;

	if (cbt->page->type != WT_PAGE_ROW_LEAF)
		return (0);

	/*
	 * For row-store pages, we need a single item that tells us the part
	 * of the page we're walking (otherwise switching from next to prev
	 * and vice-versa is just too complicated), so we map the WT_ROW and
	 * WT_INSERT_HEAD array slots into a single name space: slot 1 is the
	 * "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is
	 * WT_INSERT_HEAD[0], and so on.  This means WT_INSERT lists are
	 * odd-numbered slots, and WT_ROW array slots are even-numbered slots.
	 */
	cbt->slot = (cbt->slot + 1) * 2;
	if (cbt->ins_head != NULL) {
		if (cbt->ins_head == WT_ROW_INSERT_SMALLEST(cbt->page))
			cbt->slot = 1;
		else
			cbt->slot += 1;
	}

	/*
	 * If we're in an insert list, figure out how far in, we have to track
	 * our current slot for previous traversals.
	 */
	cbt->ins_entry_cnt = 0;
	if (cbt->ins_head != NULL)
		WT_SKIP_FOREACH(ins, cbt->ins_head) {
			++cbt->ins_entry_cnt;
			if (ins == cbt->ins)
				break;
		}

	F_CLR(cbt, WT_CBT_SEARCH_SET);
	return (0);
}

/*
 * __wt_btcur_first --
 *	Move to the first record in the tree.
 */
int
__wt_btcur_first(WT_CURSOR_BTREE *cbt)
{
	__wt_cursor_clear(cbt);

	return (__wt_btcur_next(cbt));
}

/*
 * __wt_btcur_next --
 *	Move to the next record in the tree.
 */
int
__wt_btcur_next(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int newpage, ret;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_readnext);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	/* If iterating from a search position, there's some setup to do. */
	if (F_ISSET(cbt, WT_CBT_SEARCH_SET))
		WT_RET(__wt_btcur_search_setup(cbt));

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the next page, until we reach the end of the
	 * file.
	 */
	for (newpage = 0;; newpage = 1) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_next(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_next(cbt, newpage,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __cursor_row_next(cbt, newpage);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}

		do {
			WT_ERR(__wt_tree_np(session, &cbt->page, 1));
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
