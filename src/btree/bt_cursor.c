/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __cursor_invalid --
 *	Return if the cursor references an invalid K/V pair (either the pair
 * doesn't exist at all because the tree is empty, or the pair was deleted).
 */
static inline int
__cursor_invalid(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_PAGE *page;

	btree = cbt->btree;
	ins = cbt->ins;
	page = cbt->page;

	/* If we found an item on an insert list, check there. */
	if (ins != NULL)
		return (WT_UPDATE_DELETED_ISSET(ins->upd) ? 1 : 0);

	/* The page may be empty, the search routine doesn't check. */
	if (page->entries == 0)
		return (1);

	/* Otherwise, check for an update in the page's slots. */
	switch (btree->type) {
	case BTREE_COL_FIX:
		break;
	case BTREE_COL_VAR:
		cip = &page->u.col_leaf.d[cbt->slot];
		if ((cell = WT_COL_PTR(page, cip)) == NULL)
			return (WT_NOTFOUND);
		__wt_cell_unpack(cell, &unpack);
		if (unpack.type == WT_CELL_DEL)
			return (1);
		break;
	case BTREE_ROW:
		if (page->u.row_leaf.upd != NULL &&
		    page->u.row_leaf.upd[cbt->slot] != NULL &&
		    WT_UPDATE_DELETED_ISSET(page->u.row_leaf.upd[cbt->slot]))
			return (1);
		break;
	}
	return (0);
}

/*
 * __wt_btcur_search --
 *	Search for a matching record in the tree.
 */
int
__wt_btcur_search(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, cursor_read);

	__cursor_func_init(cbt, 1);

	WT_ERR(btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) :
	    __wt_col_search(session, cbt, 0));
	if (cbt->compare != 0 || __cursor_invalid(cbt))
		ret = WT_NOTFOUND;
	else
		ret = __wt_kv_return(session, cbt, 0);

err:	__cursor_func_resolve(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, cursor_read_near);

	__cursor_func_init(cbt, 1);

	/*
	 * If we find a record that is not deleted, return it.
	 *
	 * If we find a deleted key, first try to move to the next key in the
	 * tree (bias for prefix searches).  Cursor next skips deleted records,
	 * so we don't have to test for them here.
	 *
	 * If there's no larger tree key, redo the search and try and find an
	 * earlier record.  If that fails, quit, there's no record to return.
	 */
	WT_ERR(btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) :
	    __wt_col_search(session, cbt, 0));
	if (!__cursor_invalid(cbt)) {
		*exact = cbt->compare;
		ret = __wt_kv_return(session, cbt, cbt->compare == 0 ? 0 : 1);
	} else if ((ret = __wt_btcur_next(cbt)) != WT_NOTFOUND)
		*exact = 1;
	else {
		WT_ERR(btree->type == BTREE_ROW ?
		    __wt_row_search(session, cbt, 0) :
		    __wt_col_search(session, cbt, 0));
		if (!__cursor_invalid(cbt)) {
			*exact = cbt->compare;
			ret = __wt_kv_return(
			    session, cbt, cbt->compare == 0 ? 0 : 1);
		} else if ((ret = __wt_btcur_prev(cbt)) != WT_NOTFOUND)
			*exact = -1;
	}

err:	__cursor_func_resolve(cbt, ret);
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
	WT_BSTAT_INCR(session, cursor_inserts);

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		/*
		 * If WT_CURSTD_OVERWRITE not set, insert a new record (ignoring
		 * the application's record number), return the record number.
		 *
		 * If WT_CURSTD_OVERWRITE set, insert/update the key/value pair,
		 * (where the key must be specified by the application).
		 */
		if (F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
			WT_ERR(__wt_col_search(session, cbt, 1));
			if ((ret = __wt_col_modify(session,
			    cbt, cbt->compare == 0 ? 3 : 1)) == WT_RESTART)
				goto retry;
		} else {
			if ((ret =
			    __wt_col_modify(session, cbt, 1)) == WT_RESTART)
				goto retry;
			cbt->iface.recno = cbt->recno;
		}
		break;
	case BTREE_ROW:
		/*
		 * If WT_CURSTD_OVERWRITE not set, fail if the key exists, else
		 * insert the key/value pair.
		 *
		 * If WT_CURSTD_OVERWRITE set, insert/update the key/value pair.
		 */
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare == 0 &&
		    !__cursor_invalid(cbt) &&
		    !F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
			ret = WT_DUPLICATE_KEY;
			break;
		}
		if ((ret = __wt_row_modify(session, cbt, 0)) == WT_RESTART)
			goto retry;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_func_resolve(cbt, ret);

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
	WT_BSTAT_INCR(session, cursor_removes);

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		/* Remove the record if it exists. */
		WT_ERR(__wt_col_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_col_modify(session, cbt, 2)) == WT_RESTART)
			goto retry;
		break;
	case BTREE_ROW:
		/* Remove the record if it exists. */
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_row_modify(session, cbt, 1)) == WT_RESTART)
			goto retry;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_func_resolve(cbt, ret);

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
	WT_BSTAT_INCR(session, cursor_updates);

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		/* Update the record. */
		WT_ERR(__wt_col_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			ret = WT_NOTFOUND;
		if ((ret = __wt_col_modify(session, cbt, 3)) == WT_RESTART)
			goto retry;
		break;
	case BTREE_ROW:
		/* Update the record it it exists. */
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_row_modify(session, cbt, 0)) == WT_RESTART)
			goto retry;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_func_resolve(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt, const char *cfg[])
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(cfg);
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	__cursor_func_init(cbt, 1);

	__wt_buf_free(session, &cbt->value);

	return (0);
}
