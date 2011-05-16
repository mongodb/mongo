/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

/*
 * __wt_btcur_first --
 *	Move to the first record in the tree.
 */
int
__wt_btcur_first(CURSOR_BTREE *cbt)
{
	SESSION *session;

	session = (SESSION *)cbt->iface.session;

	WT_RET(__wt_walk_begin(session, NULL, &cbt->walk, 0));
	F_SET(&cbt->iface, WT_CURSTD_POSITIONED);

	return (__wt_btcur_next(cbt));
}

static inline int
__btcur_next_fix(CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found;

	/* New page? */
	if (cbt->nitems == 0) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
	}

	/*
	 * This slightly odd-looking loop lets us have one place that does the
	 * incrementing to move through a page.
	 */
	for (found = 0; !found; ++cbt->cip, ++cbt->recno, --cbt->nitems) {
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		*recnop = cbt->recno;
		cell = WT_COL_PTR(cbt->page, cbt->cip);
		if ((upd = WT_COL_UPDATE(cbt->page, cbt->cip)) == NULL) {
			if (!WT_FIX_DELETE_ISSET(cell)) {
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
__btcur_next_rle(CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found, newcell;

	newcell = 0;

	/* New page? */
	if (cbt->nitems == 0) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
		newcell = 1;
	}

	for (;; ++cbt->cip, newcell = 1) {
		cell = WT_COL_PTR(cbt->page, cbt->cip);
		if (newcell) {
			cbt->nrepeats = WT_RLE_REPEAT_COUNT(cell);
			cbt->ins = WT_COL_INSERT(cbt->page, cbt->cip);
		}

		for (found = 0;
		    !found && cbt->nrepeats > 0;
		    ++cbt->recno, --cbt->nrepeats) {
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
		else if (--cbt->nitems == 0)
			return (WT_NOTFOUND);
	}

	/* NOTREACHED */
}

static inline int
__btcur_next_var(CURSOR_BTREE *cbt, wiredtiger_recno_t *recnop, WT_BUF *value)
{
	SESSION *session;
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found;

	session = (SESSION *)cbt->iface.session;

	/* New page? */
	if (cbt->nitems == 0) {
		cbt->cip = cbt->page->u.col_leaf.d;
		cbt->nitems = cbt->page->entries;
		cbt->recno = cbt->page->u.col_leaf.recno;
	}

	/*
	 * This slightly odd-looking loop lets us have one place that does the
	 * incrementing to move through a page.
	 */
	for (found = 0; !found; ++cbt->cip, ++cbt->recno, --cbt->nitems) {
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		/* Check for deletion. */
		upd = WT_COL_UPDATE(cbt->page, cbt->cip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		/* We've got a record. */
		found = 1;
		*recnop = cbt->recno;

		/*
		 * If the item was ever modified, use the data from the
		 * WT_UPDATE entry. Then check for empty data.  Finally, use
		 * the value from the disk image.
		 */
		if (upd != NULL) {
			value->data = WT_UPDATE_DATA(upd);
			value->size = upd->size;
		} else {
			cell = WT_COL_PTR(cbt->page, cbt->cip);
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_DATA:
				if (cbt->btree->huffman_value == NULL) {
					value->data = WT_CELL_BYTE(cell);
					value->size = WT_CELL_LEN(cell);
					break;
				}
				/* FALLTHROUGH */
			case WT_CELL_DATA_OVFL:
				WT_RET(__wt_cell_process(session, cell, value));
				break;
			case WT_CELL_DEL:
				found = 0;
				break;
			WT_ILLEGAL_FORMAT(session);
			}
		}
	}

	return (0);
}

static inline int
__btcur_next_row(CURSOR_BTREE *cbt, WT_BUF *key, WT_BUF *value)
{
	SESSION *session;
	WT_CELL *cell;
	WT_UPDATE *upd;
	int found;

	session = (SESSION *)cbt->iface.session;

	/* New page? */
	if (cbt->nitems == 0) {
		cbt->rip = cbt->page->u.row_leaf.d;
		cbt->nitems = cbt->page->entries;
	}

	/*
	 * This slightly odd-looking loop lets us have one place that does the
	 * incrementing to move through a page.
	 */
	for (found = 0; !found; ++cbt->rip, --cbt->nitems) {
		if (cbt->nitems == 0)
			return (WT_NOTFOUND);

		/* Check for deletion. */
		upd = WT_ROW_UPDATE(cbt->page, cbt->rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		/* We've got a record. */
		found = 1;

		/* Set the key. */
		if (__wt_key_process(cbt->rip))
			WT_RET(__wt_key_build(session,
			    cbt->page, cbt->rip, key));
		else {
			key->data = cbt->rip->key;
			key->size = cbt->rip->size;
		}

		/*
		 * If the item was ever modified, use the data from the
		 * WT_UPDATE entry. Then check for empty data.  Finally, use
		 * the value from the disk image.
		 */
		if (upd != NULL) {
			value->data = WT_UPDATE_DATA(upd);
			value->size = upd->size;
		} else if (WT_ROW_EMPTY_ISSET(cbt->rip)) {
			value->data = "";
			value->size = 0;
		} else {
			cell = WT_ROW_PTR(cbt->page, cbt->rip);
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_DATA:
				if (cbt->btree->huffman_value == NULL) {
					value->data = WT_CELL_BYTE(cell);
					value->size = WT_CELL_LEN(cell);
					break;
				}
				/* FALLTHROUGH */
			case WT_CELL_DATA_OVFL:
				WT_RET(__wt_cell_process(session, cell, value));
				break;
			WT_ILLEGAL_FORMAT(session);
			}
		}
	}

	return (0);
}

/*
 * __wt_btcur_next --
 *	Move to the next record in the tree.
 */
int
__wt_btcur_next(CURSOR_BTREE *cbt)
{
	SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	cursor = &cbt->iface;
	session = (SESSION *)cbt->iface.session;

	if (cbt->walk.tree == NULL)
		return (__wt_btcur_first(cbt));

	for (ret = WT_NOTFOUND; ret == WT_NOTFOUND;) {
		if (cbt->nitems == 0) {
			WT_RET(__wt_walk_next(session, &cbt->walk, &cbt->page));
			if (cbt->page == NULL) {
				ret = WT_NOTFOUND;
				break;
			}
		}

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
		default:
			continue;
		}
	}

	if (ret == 0)
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
__wt_btcur_prev(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(CURSOR_BTREE *cbt, int *exact)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	*exact = 0;
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		ret = __wt_btree_col_get(session,
		    cursor->recno, (WT_ITEM *)&cursor->value);
		break;
	case BTREE_ROW:
		ret = __wt_btree_row_get(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value);
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
__wt_btcur_insert(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		return (__wt_btree_col_put(session,
		    cursor->recno, (WT_ITEM *)&cursor->value));
	case BTREE_ROW:
		return (__wt_btree_row_put(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value));
	WT_ILLEGAL_FORMAT(session);
	}
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		return (__wt_btree_col_put(session,
		    cursor->recno, (WT_ITEM *)&cursor->value));
	case BTREE_ROW:
		return (__wt_btree_row_put(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value));
	WT_ILLEGAL_FORMAT(session);
	}
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
	case BTREE_COL_VAR:
		return (__wt_btree_col_del(session, cursor->recno));
	case BTREE_ROW:
		return (__wt_btree_row_del(session, (WT_ITEM *)&cursor->key));
	WT_ILLEGAL_FORMAT(session);
	}
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(CURSOR_BTREE *cbt, const char *config)
{
	SESSION *session;

	WT_UNUSED(config);

	session = (SESSION *)cbt->iface.session;
	__wt_walk_end(session, &cbt->walk);
	return (0);
}
