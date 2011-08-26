/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __next_fix --
 *	Move to the next, fixed-length column-store item.
 */
static inline int
__next_fix(
    WT_CURSOR_BTREE *cbt, int newpage, uint64_t *recnop, WT_BUF *value)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	int found;
	uint8_t v;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = session->btree;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->ins = WT_SKIP_FIRST(WT_COL_INSERT_SINGLE(cbt->page));
		cbt->nslots = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
	}

	/* This loop moves through a page, including after reading a record. */
	for (found = 0; !found; ++cbt->recno, --cbt->nslots) {
		if (cbt->nslots == 0)
			return (WT_NOTFOUND);

		*recnop = cbt->recno;
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
			value->size = 1;
			found = 1;
		} else {
			v = __bit_getv_recno(
			    cbt->page, cbt->recno, btree->bitcnt);
			WT_RET(__wt_buf_set(session, &cbt->value, &v, 1));
			value->data = cbt->value.data;
			value->size = 1;
			found = 1;
		}
	}

	return (0);
}

/*
 * __next_var --
 *	Move to the next, variable-length column-store item.
 */
static inline int
__next_var(WT_CURSOR_BTREE *cbt,
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
 * __next_row --
 *	Move to the next row-store item.
 */
static inline int
__next_row(WT_CURSOR_BTREE *cbt, WT_BUF *key, WT_BUF *value, int skip)
{
	WT_CELL *cell;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	WT_ROW *rip;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/*
	 * Return the entry referenced by the cursor, and then move to the
	 * next entry.
	 */
	for (;; skip = 0) {
		/* Continue traversing any insert list. */
		if ((ins = cbt->ins) != NULL) {
			++cbt->ins_entry_cnt;
			cbt->ins = WT_SKIP_NEXT(ins);

			upd = ins->upd;
			if (skip || WT_UPDATE_DELETED_ISSET(upd))
				continue;
			key->data = WT_INSERT_KEY(ins);
			key->size = WT_INSERT_KEY_SIZE(ins);
			value->data = WT_UPDATE_DATA(upd);
			value->size = upd->size;
			return (0);
		}

		/* Check to see if we've finished with this page. */
		if (cbt->slot == cbt->page->entries)
			return (WT_NOTFOUND);

		/*
		 * Set up for this slot, and any insert list that follows this
		 * slot.
		 */
		rip = &cbt->page->u.row_leaf.d[cbt->slot++];
		cbt->ins_head = WT_ROW_INSERT(cbt->page, rip);
		cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
		cbt->ins_entry_cnt = 0;

		/* If the slot has been deleted, we don't have a record. */
		upd = WT_ROW_UPDATE(cbt->page, rip);
		if (skip || (upd != NULL && WT_UPDATE_DELETED_ISSET(upd)))
			continue;

		/*
		 * Return the slot's K/V pair.
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
		if (upd != NULL) {
			value->data = WT_UPDATE_DATA(upd);
			value->size = upd->size;
		} else if ((cell = __wt_row_value(cbt->page, rip)) == NULL) {
			value->data = "";
			value->size = 0;
		} else
			WT_RET(__wt_cell_copy(session, cell, value));
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __wt_btcur_search_setup --
 *	Initialize a cursor for iteration based on a search.
 */
int
__wt_btcur_search_setup(WT_CURSOR_BTREE *cbt, int next)
{
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/*
	 * A cursor that was positioned using a search function is now being
	 * used for iteration.  We don't want the search function to be slow,
	 * so we put off doing expensive tasks until we know the application
	 * is going to iterate with the cursor.
	 *
	 * First, build a stack of pages in the tree.
	 */
	WT_RET(__wt_walk_set(session, cbt->page, &cbt->walk, 0));

	/*
	 * If we're in an insert list and moving forward through the tree, and
	 * it's the "smaller than any page key" insert list, reset the slot to
	 * 0, that's our traversal slot going forward.  If not the "smallest"
	 * insert list, increment the page slot because we've logically already
	 * returned the current slot, it appears before any insert list.
	 */
	if (cbt->ins_head != NULL && next) {
		if (cbt->ins_head == WT_ROW_INSERT_SMALLEST(cbt->page))
			cbt->slot = 0;
		else
			++cbt->slot;
	}

	/*
	 * If we're in an insert list, figure out how far in, we have to track
	 * our current slot in case we reverse direction.
	 */
	cbt->ins_entry_cnt = 0;
	if (cbt->ins_head != NULL)
		WT_SKIP_FOREACH(ins, cbt->ins_head) {
			++cbt->ins_entry_cnt;
			if (ins == cbt->ins)
				break;
		}

	/* Our page hazard reference is now a tree hazard reference. */
	F_CLR(cbt, WT_CBT_PAGE_RELEASE);
	F_SET(cbt, WT_CBT_WALK_RELEASE);

	return (0);
}

/*
 * __wt_btcur_first --
 *	Move to the first record in the tree.
 */
int
__wt_btcur_first(WT_CURSOR_BTREE *cbt)
{
	__wt_cursor_hazard_clear(cbt);

	cbt->iter_state = WT_CBT_NOTHING;

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
	int newpage, ret, skip;

	skip = 0;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_readnext);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	switch (cbt->iter_state) {
	case WT_CBT_NOTHING:
		WT_RET(__wt_walk_first(session, &cbt->walk, 0));
		break;
	case WT_CBT_SEARCH:
		WT_RET(__wt_btcur_search_setup(cbt, 1));

		/*
		 * The iteration functions return the record referenced by the
		 * cursor, and then move the cursor: setting up a search means
		 * referencing a record we've already returned (or deleted, or
		 * whatever), skip that record and move to the next record.
		 */
		skip = 1;
		break;
	case WT_CBT_ITERATING:
		break;
	}

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the next page, until we reach the end of the
	 * file.
	 */
	for (newpage = 0;; newpage = 1, skip = 0) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __next_fix(cbt, newpage,
				   &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_COL_VAR:
				ret = __next_var(cbt, newpage,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __next_row(
				    cbt, &cursor->key, &cursor->value, skip);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}

		do {
			WT_ERR(__wt_walk_next(session, &cbt->walk, &cbt->page));
			WT_ERR_TEST(cbt->page == NULL, WT_NOTFOUND);
		} while (
		    cbt->page->type == WT_PAGE_COL_INT ||
		    cbt->page->type == WT_PAGE_ROW_INT);

		switch (cbt->page->type) {
		case WT_PAGE_ROW_LEAF:
			cbt->ins_head = WT_ROW_INSERT_SMALLEST(cbt->page);
			cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
			cbt->slot = 0;
			break;
		}
	}

err:	if (ret != 0)
		return (ret);

	F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	cbt->iter_state = WT_CBT_ITERATING;
	return (0);
}
