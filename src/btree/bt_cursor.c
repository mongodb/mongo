/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cursor_size_chk --
 *	Return if an inserted item is too large.
 */
static inline int
__cursor_size_chk(WT_SESSION_IMPL *session, WT_ITEM *kv)
{
	WT_BTREE *btree;

	btree = session->btree;

	if (btree->type == BTREE_COL_FIX) {
		/* Fixed-size column-stores take a single byte. */
		if (kv->size != 1)
			WT_RET_MSG(session, EINVAL,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    kv->size);
	} else {
		if (kv->size > WT_BTREE_MAX_OBJECT_SIZE)
			WT_RET_MSG(session, EINVAL,
			    "item size of %" PRIu32 " exceeds the maximum "
			    "supported size of %" PRIu32,
			    kv->size, WT_BTREE_MAX_OBJECT_SIZE);
	}
	return (0);
}

/*
 * __cursor_fix_implicit --
 *	Return if search went past the end of the tree.
 */
static inline int
__cursor_fix_implicit(WT_BTREE *btree, WT_CURSOR_BTREE *cbt)
{
	return (btree->type == BTREE_COL_FIX &&
	    !F_ISSET(cbt, WT_CBT_MAX_RECORD) ? 1 : 0);
}

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
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	btree = cbt->btree;
	ins = cbt->ins;
	page = cbt->page;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/* If we found an item on an insert list, check there. */
	if (ins != NULL && (upd = __wt_txn_read(session, ins->upd)) != NULL)
		return (WT_UPDATE_DELETED_ISSET(upd) ? 1 : 0);

	/* The page may be empty, the search routine doesn't check. */
	if (page->entries == 0)
		return (1);

	/* Otherwise, check for an update in the page's slots. */
	switch (btree->type) {
	case BTREE_COL_FIX:
		break;
	case BTREE_COL_VAR:
		cip = &page->u.col_var.d[cbt->slot];
		if ((cell = WT_COL_PTR(page, cip)) == NULL)
			return (WT_NOTFOUND);
		__wt_cell_unpack(cell, &unpack);
		if (unpack.type == WT_CELL_DEL)
			return (1);
		break;
	case BTREE_ROW:
		if (page->u.row.upd != NULL && (upd = __wt_txn_read(session,
		    page->u.row.upd[cbt->slot])) != NULL &&
		    WT_UPDATE_DELETED_ISSET(upd))
			return (1);
		break;
	}
	return (0);
}

/*
 * __wt_btcur_reset --
 *	Invalidate the cursor position.
 */
int
__wt_btcur_reset(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	WT_BSTAT_INCR(session, cursor_resets);

	__cursor_func_init(cbt, 1);
	__cursor_search_clear(cbt);

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
	WT_DECL_RET;
	WT_ITEM *val;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, cursor_read);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

	__cursor_func_init(cbt, 1);

	WT_ERR(btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) :
	    __wt_col_search(session, cbt, 0));
	if (cbt->compare != 0 || __cursor_invalid(cbt)) {
		/*
		 * Creating a record past the end of the tree in a fixed-length
		 * column-store implicitly fills the gap with empty records.
		 */
		if (__cursor_fix_implicit(btree, cbt)) {
			cbt->v = 0;
			val = &cbt->iface.value;
			val->data = &cbt->v;
			val->size = 1;
		} else
			ret = WT_NOTFOUND;
	} else
		ret = __wt_kv_return(session, cbt);

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
	WT_DECL_RET;
	WT_ITEM *val;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, cursor_read_near);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

	__cursor_func_init(cbt, 1);

	WT_ERR(btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) :
	    __wt_col_search(session, cbt, 0));

	/*
	 * Creating a record past the end of the tree in a fixed-length column-
	 * store implicitly fills the gap with empty records.  In this case, we
	 * instantiate the empty record, it's an exact match.
	 *
	 * Else, if we find a valid key (one that wasn't deleted), return it.
	 *
	 * Else, if we found a deleted key, try to move to the next key in the
	 * tree (bias for prefix searches).  Cursor next skips deleted records,
	 * so we don't have to test for them again.
	 *
	 * Else if there's no larger tree key, redo the search and try and find
	 * an earlier record.  If that fails, quit, there's no record to return.
	 */
	if (cbt->compare != 0 && __cursor_fix_implicit(btree, cbt)) {
		cbt->v = 0;
		val = &cbt->iface.value;
		val->data = &cbt->v;
		val->size = 1;
		*exact = 0;
	} else if (!__cursor_invalid(cbt)) {
		*exact = cbt->compare;
		ret = __wt_kv_return(session, cbt);
	} else if ((ret = __wt_btcur_next(cbt)) != WT_NOTFOUND)
		*exact = 1;
	else {
		WT_ERR(btree->type == BTREE_ROW ?
		    __wt_row_search(session, cbt, 0) :
		    __wt_col_search(session, cbt, 0));
		if (!__cursor_invalid(cbt)) {
			*exact = cbt->compare;
			ret = __wt_kv_return(session, cbt);
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, cursor_inserts);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		/*
		 * If WT_CURSTD_APPEND is set, insert a new record (ignoring
		 * the application's record number).  First we search for the
		 * maximum possible record number so the search ends on the
		 * last page.  The real record number is assigned by the
		 * serialized append operation.
		 * __wt_col_append_serial_func
		 */
		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = UINT64_MAX;

		WT_ERR(__wt_col_search(session, cbt, 1));

		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = 0;

		/*
		 * If WT_CURSTD_OVERWRITE set, insert/update the key/value pair.
		 *
		 * If WT_CURSTD_OVERWRITE not set, fail if the key exists, else
		 * insert the key/value pair.  Creating a record past the end
		 * of the tree in a fixed-length column-store implicitly fills
		 * the gap with empty records.  Fail in that case, the record
		 * exists.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    ((cbt->compare == 0 && !__cursor_invalid(cbt)) ||
		    (cbt->compare != 0 && __cursor_fix_implicit(btree, cbt)))) {
			ret = WT_DUPLICATE_KEY;
			break;
		}
		if ((ret = __wt_col_modify(session, cbt, 3)) == WT_RESTART)
			goto retry;
		if (F_ISSET(cursor, WT_CURSTD_APPEND) && ret == 0)
			cbt->iface.recno = cbt->recno;
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
	WT_ILLEGAL_VALUE_ERR(session);
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, cursor_removes);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__wt_col_search(session, cbt, 1));

		/*
		 * Remove the record if it exists.  Creating a record past the
		 * end of the tree in a fixed-length column-store implicitly
		 * fills the gap with empty records.  Return success in that
		 * case, the record was deleted successfully.
		 */
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			ret =
			    __cursor_fix_implicit(btree, cbt) ? 0 : WT_NOTFOUND;
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
	WT_ILLEGAL_VALUE_ERR(session);
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, cursor_updates);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

retry:	__cursor_func_init(cbt, 1);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1)
			WT_RET_MSG(session, EINVAL,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		WT_ERR(__wt_col_search(session, cbt, 1));

		/*
		 * Update the record if it exists.  Creating a record past the
		 * end of the tree in a fixed-length column-store implicitly
		 * fills the gap with empty records.  Update the record in that
		 * case, the record exists.
		 */
		if ((cbt->compare != 0 || __cursor_invalid(cbt)) &&
		    !__cursor_fix_implicit(btree, cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_col_modify(session, cbt, 3)) == WT_RESTART)
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
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	__cursor_func_resolve(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	__cursor_func_init(cbt, 1);
	__wt_buf_free(session, &cbt->tmp);

	return (0);
}
