/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btcur_first --
 *	Move to the first record in the tree.
 */
int
__wt_btcur_first(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_RET(__wt_walk_begin(session, NULL, &cbt->walk, 0));
	F_SET(&cbt->iface, WT_CURSTD_POSITIONED);

	return (__wt_btcur_next(cbt));
}

static inline int
__btcur_next_fix(
    WT_CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found;

	/* Initialize for each new page. */
	if (cbt->cip == NULL) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
	}

	/* This loop moves through a page, including after reading a record. */
	for (found = 0; !found; ++cbt->cip, ++cbt->recno, --cbt->nitems) {
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		*recnop = cbt->recno;
		if ((upd = WT_COL_UPDATE(cbt->page, cbt->cip)) == NULL) {
			cell = WT_COL_PTR(cbt->page, cbt->cip);
			if (cell != NULL && !WT_FIX_DELETE_ISSET(cell)) {
				value->data = cell;
				value->size = cbt->btree->fixed_len;
				found = 1;
			}
		} else if (!WT_UPDATE_DELETED_ISSET(upd)) {
			value->data = WT_UPDATE_DATA(upd);
			value->size = cbt->btree->fixed_len;
			found = 1;
		}
	}

	return (0);
}

static inline int
__btcur_next_rle(
    WT_CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found, newcell;

	/* Initialize for each new page. */
	if (cbt->cip == NULL) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
		newcell = 1;
	} else
		newcell = 0;

	for (;; ++cbt->cip, newcell = 1) {
		if ((cell = WT_COL_PTR(cbt->page, cbt->cip)) == NULL)
			goto deleted;
		if (newcell) {
			cbt->rle = WT_RLE_REPEAT_COUNT(cell);
			cbt->ins = WT_COL_INSERT(cbt->page, cbt->cip);
		}

		for (found = 0;
		    !found && cbt->rle > 0;
		    ++cbt->recno, --cbt->rle) {
			*recnop = cbt->recno;
			if (cbt->ins != NULL &&
			    WT_INSERT_RECNO(cbt->ins) == *recnop) {
				upd = cbt->ins->upd;
				if (!WT_UPDATE_DELETED_ISSET(upd)) {
					value->data = WT_UPDATE_DATA(upd);
					value->size = upd->size;
					found = 1;
				}
				cbt->ins = cbt->ins->next;
			} else if (!WT_FIX_DELETE_ISSET(
			    WT_RLE_REPEAT_DATA(cell))) {
				value->data = WT_RLE_REPEAT_DATA(cell);
				value->size = cbt->btree->fixed_len;
				found = 1;
			}
		}

		if (found)
			return (0);

deleted:	if (--cbt->nitems == 0)
			return (WT_NOTFOUND);
	}

	/* NOTREACHED */
}

static inline int
__btcur_next_var(
    WT_CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	WT_SESSION_IMPL *session;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_UPDATE *upd;
	int newcell;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	unpack = &_unpack;

	/* Initialize for each new page. */
	if (cbt->cip == NULL) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
		newcell = 1;
	} else
		newcell = 0;

	/* This loop moves through a page. */
	for (;; ++cbt->cip, --cbt->nitems, newcell = 1) {
		/* Check for the end of the page */
		if (cbt->nitems == 0)
			break;

		/* Unpack each cell, find out how many times it's repeated. */
		if (newcell) {
			if ((cell = WT_COL_PTR(cbt->page, cbt->cip)) != NULL) {
				__wt_cell_unpack(cell, unpack);
				cbt->rle = unpack->rle;
			} else
				cbt->rle = 1;

			cbt->ins = WT_COL_INSERT(cbt->page, cbt->cip);

			/*
			 * Skip deleted records, there might be a large number
			 * of them.
			 */
			if (cbt->ins == NULL &&
			    (cell == NULL || unpack->type == WT_CELL_DEL)) {
				cbt->recno += cbt->rle;
				continue;
			}

			/*
			 * Get a copy of the item we're returning: it might be
			 * encoded, and we don't want to repeatedly decode it.
			 */
			if (cell != NULL)
				WT_RET(__wt_cell_unpack_copy(
				    session, unpack, &cbt->value));
		}

		/* Return the data RLE-count number of times. */
		while (cbt->rle > 0) {
			--cbt->rle;
			*recnop = cbt->recno++;

			/*
			 * Check any insert list for a matching record (insert
			 * lists are in sorted order, we check the next entry).
			 */
			if (cbt->ins != NULL &&
			    WT_INSERT_RECNO(cbt->ins) == cbt->recno) {
				upd = cbt->ins->upd;
				cbt->ins = cbt->ins->next;

				if (WT_UPDATE_DELETED_ISSET(upd))
					continue;
				value->data = WT_UPDATE_DATA(upd);
				value->size = upd->size;
			} else {
				if (cell == NULL)
					continue;
				value->data = cbt->value.data;
				value->size = cbt->value.size;
			}
			return (0);
		}
	}
	return (WT_NOTFOUND);
}

static inline int
__btcur_next_row(WT_CURSOR_BTREE *cbt, WT_BUF *key, WT_BUF *value)
{
	WT_CELL *cell;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	int found;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/* Initialize for each new page. */
	if (cbt->rip == NULL) {
		cbt->rip = cbt->page->u.row_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->ins = WT_ROW_INSERT_SMALLEST(cbt->page);
	}

	/* This loop moves through a page, including after reading a record. */
	for (found = 0; !found;
	    cbt->ins = WT_ROW_INSERT(cbt->page, cbt->rip),
	    ++cbt->rip, --cbt->nitems) {
		/* Continue traversing any insert list. */
		while ((ins = cbt->ins) != NULL) {
			cbt->ins = cbt->ins->next;
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
 * __wt_btcur_next --
 *	Move to the next record in the tree.
 */
int
__wt_btcur_next(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	int ret;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (cbt->walk.tree == NULL)
		return (__wt_btcur_first(cbt));

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the next page, until we reach the end of the
	 * file.
	 */
	for (;;) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __btcur_next_fix(cbt,
				   &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_COL_RLE:
				ret = __btcur_next_rle(cbt,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_COL_VAR:
				ret = __btcur_next_var(cbt,
				    &cursor->recno, &cursor->value);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __btcur_next_row(cbt,
				    &cursor->key, &cursor->value);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}
		cbt->cip = NULL;
		cbt->rip = NULL;

		do {
			WT_ERR(__wt_walk_next(session, &cbt->walk, &cbt->page));
			WT_ERR_TEST(cbt->page == NULL, WT_NOTFOUND);
		} while (
		    cbt->page->type == WT_PAGE_COL_INT ||
		    cbt->page->type == WT_PAGE_ROW_INT);
	}

err:	if (ret == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		F_CLR(cursor, WT_CURSTD_POSITIONED |
		    WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	return (ret);
}

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	*exact = 0;
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		while ((ret = __wt_col_search(
		    session, cursor->recno, 0)) == WT_RESTART)
			;
		if (ret == 0) {
			ret = __wt_return_data(session,
			    NULL, (WT_ITEM *)&cursor->value, 0);
			WT_PAGE_OUT(session, session->srch_page);
		}
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_search(
		    session, (WT_ITEM *)&cursor->key, 0)) == WT_RESTART)
			;
		if (ret == 0) {
			ret = __wt_return_data(session,
			    (WT_ITEM *)&cursor->key,
			    (WT_ITEM *)&cursor->value, 0);
			WT_PAGE_OUT(session, session->srch_page);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	if (ret == 0)
		F_SET(cursor, WT_CURSTD_VALUE_SET);

	return (ret);
}

/*
 * __wt_btcur_insert --
 *	Insert a record into the tree.
 */
int
__wt_btcur_insert(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(session,
		    cursor->recno, (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(session,
		    (WT_ITEM *)&cursor->key,
		    (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (((WT_ITEM *)&cursor->value)->size != btree->fixed_len) {
			__wt_errx(session, "length of %" PRIu32
			    " does not match fixed-length file configuration "
			    "of %" PRIu32,
			    ((WT_ITEM *)&cursor->value)->size,
			    btree->fixed_len);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(session,
		    cursor->recno, (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(session,
		    (WT_ITEM *)&cursor->key,
		    (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(
		    session, cursor->recno, NULL, 0)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(
		    session, (WT_ITEM *)&cursor->key, NULL, 0)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt, const char *config)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(config);

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	__wt_walk_end(session, &cbt->walk);
	__wt_buf_free(session, &cbt->value);
	return (0);
}
