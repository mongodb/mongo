/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	size_t size;

	btree = S2BT(session);
	bm = btree->bm;

	if (btree->type == BTREE_COL_FIX) {
		/* Fixed-size column-stores take a single byte. */
		if (kv->size != 1)
			WT_RET_MSG(session, EINVAL,
			    "item size of %" WT_SIZET_FMT " does not match "
			    "fixed-length file requirement of 1 byte",
			    kv->size);
		return (0);
	}

	/* Don't waste effort, 1GB is always cool. */
	if (kv->size <= WT_GIGABYTE)
		return (0);

	/*
	 * There are two checks: what we are willing to store in the tree, and
	 * what the block manager can actually write.
	 */
	if (kv->size > WT_BTREE_MAX_OBJECT_SIZE)
		ret = EINVAL;
	else {
		size = kv->size;
		ret = bm->write_size(bm, session, &size);
	}
	if (ret != 0)
		WT_RET_MSG(session, ret,
		    "item size of %" WT_SIZET_FMT " exceeds the maximum "
		    "supported size",
		    kv->size);
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
 * __cursor_valid --
 *	Return if the cursor references an valid key/value pair.
 */
static inline int
__cursor_valid(WT_CURSOR_BTREE *cbt, WT_UPDATE **updp)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_COL *cip;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	btree = cbt->btree;
	page = cbt->ref->page;
	session = (WT_SESSION_IMPL *)cbt->iface.session;
	if (updp != NULL)
		*updp = NULL;

	/*
	 * We may be pointing to an insert object, and we may have a page with
	 * existing entries.  Insert objects always have associated update
	 * objects (the value).  Any update object may be deleted, or invisible
	 * to us.  In the case of an on-page entry, there is by definition a
	 * value that is visible to us, the original page cell.
	 *
	 * If we find a visible update structure, return our caller a reference
	 * to it because we don't want to repeatedly search for the update, it
	 * might suddenly become invisible (imagine a read-uncommitted session
	 * with another session's aborted insert), and we don't want to handle
	 * that potential error every time we look at the value.
	 *
	 * Unfortunately, the objects we might have and their relationships are
	 * different for the underlying page types.
	 *
	 * In the case of row-store, an insert object implies ignoring any page
	 * objects, no insert object can have the same key as an on-page object.
	 * For row-store:
	 *	if there's an insert object:
	 *		if there's a visible update:
	 *			exact match
	 *		else
	 *			no exact match
	 *	else
	 *		use the on-page object (which may have an associated
	 *		update object that may or may not be visible to us).
	 *
	 * Column-store is more complicated because an insert object can have
	 * the same key as an on-page object: updates to column-store rows
	 * are insert/object pairs, and an invisible update isn't the end as
	 * there may be an on-page object that is visible.  This changes the
	 * logic to:
	 *	if there's an insert object:
	 *		if there's a visible update:
	 *			exact match
	 *		else if the on-page object's key matches the insert key
	 *			use the on-page object
	 *	else
	 *		use the on-page object
	 *
	 * First, check for an insert object with a visible update (a visible
	 * update that's been deleted is not a valid key/value pair).
	 */
	if (cbt->ins != NULL &&
	    (upd = __wt_txn_read(session, cbt->ins->upd)) != NULL) {
		if (WT_UPDATE_DELETED_ISSET(upd))
			return (0);
		if (updp != NULL)
			*updp = upd;
		return (1);
	}

	/*
	 * If we don't have an insert object, or in the case of column-store,
	 * there's an insert object but no update was visible to us and the key
	 * on the page is the same as the insert object's key, and the slot as
	 * set by the search function is valid, we can use the original page
	 * information.
	 */
	switch (btree->type) {
	case BTREE_COL_FIX:
		/*
		 * If search returned an insert object, there may or may not be
		 * a matching on-page object, we have to check.  Fixed-length
		 * column-store pages don't have slots, but map one-to-one to
		 * keys, check for retrieval past the end of the page.
		 */
		if (cbt->recno >= page->pg_fix_recno + page->pg_fix_entries)
			return (0);

		/*
		 * Updates aren't stored on the page, an update would have
		 * appeared as an "insert" object; no further checks to do.
		 */
		break;
	case BTREE_COL_VAR:
		/*
		 * If search returned an insert object, there may or may not be
		 * a matching on-page object, we have to check.  Variable-length
		 * column-store pages don't map one-to-one to keys, but have
		 * "slots", check if search returned a valid slot.
		 */
		if (cbt->slot >= page->pg_var_entries)
			return (0);

		/*
		 * Updates aren't stored on the page, an update would have
		 * appeared as an "insert" object; however, variable-length
		 * column store deletes are written into the backing store,
		 * check the cell for a record already deleted when read.
		 */
		cip = &page->pg_var_d[cbt->slot];
		if ((cell = WT_COL_PTR(page, cip)) == NULL ||
		    __wt_cell_type(cell) == WT_CELL_DEL)
			return (0);
		break;
	case BTREE_ROW:
		/*
		 * See above: for row-store, no insert object can have the same
		 * key as an on-page object, we're done.
		 */
		if (cbt->ins != NULL)
			return (0);

		/*
		 * Check if searched returned a valid slot (the failure mode is
		 * an empty page, the search function doesn't check, and so the
		 * more exact test is "page->pg_row_entries == 0", but this test
		 * mirrors the column-store test).
		 */
		if (cbt->slot >= page->pg_row_entries)
			return (0);

		/* Updates are stored on the page, check for a delete. */
		if (page->pg_row_upd != NULL && (upd = __wt_txn_read(
		    session, page->pg_row_upd[cbt->slot])) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				return (0);
			if (updp != NULL)
				*updp = upd;
		}
		break;
	}
	return (1);
}

/*
 * __cursor_col_search --
 *	Column-store search from an application cursor.
 */
static inline int
__cursor_col_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_DECL_RET;

	WT_WITH_PAGE_INDEX(session, 
	    ret = __wt_col_search(session, cbt->iface.recno, NULL, cbt));
	return (ret);
}

/*
 * __cursor_row_search --
 *	Row-store search from an application cursor.
 */
static inline int
__cursor_row_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int insert)
{
	WT_DECL_RET;

	WT_WITH_PAGE_INDEX(session, 
	    ret = __wt_row_search(session, &cbt->iface.key, NULL, cbt, insert));
	return (ret);
}

/*
 * __cursor_col_modify --
 *	Column-store delete, insert, and update from an application cursor.
 */
static inline int
__cursor_col_modify(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	return (__wt_col_modify(session,
	    cbt, cbt->iface.recno, &cbt->iface.value, NULL, is_remove));
}

/*
 * __cursor_row_modify --
 *	Row-store insert, update and delete from an application cursor.
 */
static inline int
__cursor_row_modify(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	return (__wt_row_modify(session,
	    cbt, &cbt->iface.key, &cbt->iface.value, NULL, is_remove));
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

	WT_STAT_FAST_CONN_INCR(session, cursor_reset);
	WT_STAT_FAST_DATA_INCR(session, cursor_reset);

	return (__cursor_reset(cbt));
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
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_STAT_FAST_CONN_INCR(session, cursor_search);
	WT_STAT_FAST_DATA_INCR(session, cursor_search);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

	WT_RET(__cursor_func_init(cbt, 1));

	WT_ERR(btree->type == BTREE_ROW ?
	    __cursor_row_search(session, cbt, 0) :
	    __cursor_col_search(session, cbt));
	if (cbt->compare == 0 && __cursor_valid(cbt, &upd))
		ret = __wt_kv_return(session, cbt, upd);
	else if (__cursor_fix_implicit(btree, cbt)) {
		/*
		 * Creating a record past the end of the tree in a fixed-length
		 * column-store implicitly fills the gap with empty records.
		 */
		cbt->recno = cursor->recno;
		cbt->v = 0;
		cursor->value.data = &cbt->v;
		cursor->value.size = 1;
	} else
		ret = WT_NOTFOUND;

err:	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
	return (ret);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exactp)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	int exact;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	exact = 0;

	WT_STAT_FAST_CONN_INCR(session, cursor_search_near);
	WT_STAT_FAST_DATA_INCR(session, cursor_search_near);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

	WT_RET(__cursor_func_init(cbt, 1));

	/*
	 * Set the "insert" flag for the btree row-store search; we may intend
	 * to position our cursor at the end of the tree, rather than match an
	 * existing record.
	 */
	WT_ERR(btree->type == BTREE_ROW ?
	    __cursor_row_search(session, cbt, 1) :
	    __cursor_col_search(session, cbt));

	/*
	 * If we find an valid key, return it.
	 *
	 * Else, creating a record past the end of the tree in a fixed-length
	 * column-store implicitly fills the gap with empty records.  In this
	 * case, we instantiate the empty record, it's an exact match.
	 *
	 * Else, move to the next key in the tree (bias for prefix searches).
	 * Cursor next skips invalid rows, so we don't have to test for them
	 * again.
	 *
	 * Else, redo the search and move to the previous key in the tree.
	 * Cursor previous skips invalid rows, so we don't have to test for
	 * them again.
	 *
	 * If that fails, quit, there's no record to return.
	 */
	if (__cursor_valid(cbt, &upd)) {
		exact = cbt->compare;
		ret = __wt_kv_return(session, cbt, upd);
	} else if (__cursor_fix_implicit(btree, cbt)) {
		cbt->recno = cursor->recno;
		cbt->v = 0;
		cursor->value.data = &cbt->v;
		cursor->value.size = 1;
		exact = 0;
	} else if ((ret = __wt_btcur_next(cbt, 0)) != WT_NOTFOUND)
		exact = 1;
	else {
		WT_ERR(btree->type == BTREE_ROW ?
		    __cursor_row_search(session, cbt, 1) :
		    __cursor_col_search(session, cbt));
		if (__cursor_valid(cbt, &upd)) {
			exact = cbt->compare;
			ret = __wt_kv_return(session, cbt, upd);
		} else if ((ret = __wt_btcur_prev(cbt, 0)) != WT_NOTFOUND)
			exact = -1;
	}

err:	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
	if (exactp != NULL && (ret == 0 || ret == WT_NOTFOUND))
		*exactp = exact;
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

	WT_STAT_FAST_CONN_INCR(session, cursor_insert);
	WT_STAT_FAST_DATA_INCR(session, cursor_insert);
	WT_STAT_FAST_DATA_INCRV(session,
	    cursor_insert_bytes, cursor->key.size + cursor->value.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

	/*
	 * The tree is no longer empty: eviction should pay attention to it,
	 * and it's no longer possible to bulk-load into it.
	 */
	if (btree->bulk_load_ok) {
		btree->bulk_load_ok = 0;
		__wt_btree_evictable(session, 1);
	}

retry:	WT_RET(__cursor_func_init(cbt, 1));

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		/*
		 * If WT_CURSTD_APPEND is set, insert a new record (ignoring
		 * the application's record number).  First we search for the
		 * maximum possible record number so the search ends on the
		 * last page.  The real record number is assigned by the
		 * serialized append operation.
		 */
		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = UINT64_MAX;

		WT_ERR(__cursor_col_search(session, cbt));

		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = 0;

		/*
		 * If not overwriting, fail if the key exists.  Creating a
		 * record past the end of the tree in a fixed-length
		 * column-store implicitly fills the gap with empty records.
		 * Fail in that case, the record exists.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    ((cbt->compare == 0 && __cursor_valid(cbt, NULL)) ||
		    (cbt->compare != 0 && __cursor_fix_implicit(btree, cbt))))
			WT_ERR(WT_DUPLICATE_KEY);

		WT_ERR(__cursor_col_modify(session, cbt, 0));
		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = cbt->recno;
		break;
	case BTREE_ROW:
		WT_ERR(__cursor_row_search(session, cbt, 1));
		/*
		 * If not overwriting, fail if the key exists, else insert the
		 * key/value pair.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    cbt->compare == 0 && __cursor_valid(cbt, NULL))
			WT_ERR(WT_DUPLICATE_KEY);

		ret = __cursor_row_modify(session, cbt, 0);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;
	/* Insert doesn't maintain a position across calls, clear resources. */
	if (ret == 0)
		WT_TRET(__curfile_leave(cbt));
	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
	return (ret);
}

/*
 * __curfile_update_check --
 *	Check whether an update would conflict.
 *
 *	This function expects the cursor to already be positioned.  It should
 *	be called before deciding whether to skip an update operation based on
 *	existence of a visible update for a key -- even if there is no value
 *	visible to the transaction, an update could still conflict.
 */
static int
__curfile_update_check(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (cbt->compare != 0)
		return (0);
	if (cbt->ins != NULL)
		return (__wt_txn_update_check(session, cbt->ins->upd));
	if (btree->type == BTREE_ROW && cbt->ref->page->pg_row_upd != NULL)
		return (__wt_txn_update_check(
		    session, cbt->ref->page->pg_row_upd[cbt->slot]));
	return (0);
}

/*
 * __wt_btcur_update_check --
 *	Check whether an update would conflict.
 *
 *	This can be used to replace WT_CURSOR::insert or WT_CURSOR::update, so
 *	they only check for conflicts without updating the tree.  It is used to
 *	maintain snapshot isolation for transactions that span multiple chunks
 *	in an LSM tree.
 */
int
__wt_btcur_update_check(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cursor = &cbt->iface;
	btree = cbt->btree;
	session = (WT_SESSION_IMPL *)cursor->session;

retry:	WT_RET(__cursor_func_init(cbt, 1));

	switch (btree->type) {
	case BTREE_ROW:
		WT_ERR(__cursor_row_search(session, cbt, 1));

		/*
		 * Just check for conflicts.
		 */
		ret = __curfile_update_check(cbt);
		break;
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;
	WT_TRET(__curfile_leave(cbt));
	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
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

	WT_STAT_FAST_CONN_INCR(session, cursor_remove);
	WT_STAT_FAST_DATA_INCR(session, cursor_remove);
	WT_STAT_FAST_DATA_INCRV(session, cursor_remove_bytes, cursor->key.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

retry:	WT_RET(__cursor_func_init(cbt, 1));

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__cursor_col_search(session, cbt));

		/*
		 * If we find a matching record, check whether an update would
		 * conflict.  Do this before checking if the update is visible
		 * in __cursor_valid, or we can miss conflict.
		 */
		WT_ERR(__curfile_update_check(cbt));

		/* Remove the record if it exists. */
		if (cbt->compare != 0 || !__cursor_valid(cbt, NULL)) {
			if (!__cursor_fix_implicit(btree, cbt))
				WT_ERR(WT_NOTFOUND);
			/*
			 * Creating a record past the end of the tree in a
			 * fixed-length column-store implicitly fills the
			 * gap with empty records.  Return success in that
			 * case, the record was deleted successfully.
			 *
			 * Correct the btree cursor's location: the search
			 * will have pointed us at the previous/next item,
			 * and that's not correct.
			 */
			cbt->recno = cursor->recno;
		} else
			ret = __cursor_col_modify(session, cbt, 1);
		break;
	case BTREE_ROW:
		/* Remove the record if it exists. */
		WT_ERR(__cursor_row_search(session, cbt, 0));

		/* Check whether an update would conflict. */
		WT_ERR(__curfile_update_check(cbt));

		if (cbt->compare != 0 || !__cursor_valid(cbt, NULL))
			WT_ERR(WT_NOTFOUND);

		ret = __cursor_row_modify(session, cbt, 1);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;
	/*
	 * If the cursor is configured to overwrite and the record is not
	 * found, that is exactly what we want.
	 */
	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) && ret == WT_NOTFOUND)
		ret = 0;

	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));

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

	WT_STAT_FAST_CONN_INCR(session, cursor_update);
	WT_STAT_FAST_DATA_INCR(session, cursor_update);
	WT_STAT_FAST_DATA_INCRV(
	    session, cursor_update_bytes, cursor->value.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

	/*
	 * The tree is no longer empty: eviction should pay attention to it,
	 * and it's no longer possible to bulk-load into it.
	 */
	if (btree->bulk_load_ok) {
		btree->bulk_load_ok = 0;
		__wt_btree_evictable(session, 1);
	}

retry:	WT_RET(__cursor_func_init(cbt, 1));

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__cursor_col_search(session, cbt));

		/*
		 * If not overwriting, fail if the key doesn't exist.  If we
		 * find an update for the key, check for conflicts.  Update the
		 * record if it exists.  Creating a record past the end of the
		 * tree in a fixed-length column-store implicitly fills the gap
		 * with empty records.  Update the record in that case, the
		 * record exists.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
			WT_ERR(__curfile_update_check(cbt));
			if ((cbt->compare != 0 || !__cursor_valid(cbt, NULL)) &&
			    !__cursor_fix_implicit(btree, cbt))
				WT_ERR(WT_NOTFOUND);
		}
		ret = __cursor_col_modify(session, cbt, 0);
		break;
	case BTREE_ROW:
		WT_ERR(__cursor_row_search(session, cbt, 1));
		/*
		 * If not overwriting, check for conflicts and fail if the key
		 * does not exist.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
			WT_ERR(__curfile_update_check(cbt));
			if (cbt->compare != 0 || !__cursor_valid(cbt, NULL))
				WT_ERR(WT_NOTFOUND);
		}
		ret = __cursor_row_modify(session, cbt, 0);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;

	/*
	 * If successful, point the cursor at internal copies of the data.  We
	 * could shuffle memory in the cursor so the key/value pair are in local
	 * buffer memory, but that's a data copy.  We don't want to do another
	 * search (and we might get a different update structure if we race).
	 * To make this work, we add a field to the btree cursor to pass back a
	 * pointer to the modify function's allocated update structure.
	 */
	if (ret == 0)
		WT_TRET(__wt_kv_return(session, cbt, cbt->modify_update));

	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
	return (ret);
}

/*
 * __wt_btcur_next_random --
 *	Move to a random record in the tree.
 */
int
__wt_btcur_next_random(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = cbt->btree;

	/*
	 * Only supports row-store: applications can trivially select a random
	 * value from a column-store, if there were any reason to do so.
	 */
	if (btree->type != BTREE_ROW)
		WT_RET(ENOTSUP);

	WT_STAT_FAST_CONN_INCR(session, cursor_next);
	WT_STAT_FAST_DATA_INCR(session, cursor_next);

	WT_RET(__cursor_func_init(cbt, 1));

	WT_WITH_PAGE_INDEX(session,
	    ret = __wt_row_random(session, cbt));
	WT_ERR(ret);
	if (__cursor_valid(cbt, &upd))
		WT_ERR(__wt_kv_return(session, cbt, upd));
	else
		WT_ERR(__wt_btcur_search_near(cbt, 0));

err:	if (ret != 0)
		WT_TRET(__cursor_reset(cbt));
	return (ret);
}

/*
 * __wt_btcur_compare --
 *	Return a comparison between two cursors.
 */
int
__wt_btcur_compare(WT_CURSOR_BTREE *a_arg, WT_CURSOR_BTREE *b_arg, int *cmpp)
{
	WT_CURSOR *a, *b;
	WT_SESSION_IMPL *session;

	a = (WT_CURSOR *)a_arg;
	b = (WT_CURSOR *)b_arg;
	session = (WT_SESSION_IMPL *)a->session;

	/* Confirm both cursors reference the same object. */
	if (a_arg->btree != b_arg->btree)
		WT_RET_MSG(
		    session, EINVAL, "Cursors must reference the same object");

	switch (a_arg->btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		/*
		 * Compare the interface's cursor record, not the underlying
		 * cursor reference: the interface's cursor reference is the
		 * one being returned to the application.
		 */
		if (a->recno < b->recno)
			*cmpp = -1;
		else if (a->recno == b->recno)
			*cmpp = 0;
		else
			*cmpp = 1;
		break;
	case BTREE_ROW:
		WT_RET(__wt_compare(
		    session, a_arg->btree->collator, &a->key, &b->key, cmpp));
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * __cursor_equals --
 *	Return if two cursors reference the same row.
 */
static inline int
__cursor_equals(WT_CURSOR_BTREE *a, WT_CURSOR_BTREE *b)
{
	switch (a->btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		/*
		 * Compare the interface's cursor record, not the underlying
		 * cursor reference: the interface's cursor reference is the
		 * one being returned to the application.
		 */
		if (((WT_CURSOR *)a)->recno == ((WT_CURSOR *)b)->recno)
			return (1);
		break;
	case BTREE_ROW:
		if (a->ref != b->ref)
			return (0);
		if (a->ins != NULL || b->ins != NULL) {
			if (a->ins == b->ins)
				return (1);
			break;
		}
		if (a->slot == b->slot)
			return (1);
		break;
	}
	return (0);
}

/*
 * __wt_btcur_equals --
 *	Return an equality comparison between two cursors.
 */
int
__wt_btcur_equals(
    WT_CURSOR_BTREE *a_arg, WT_CURSOR_BTREE *b_arg, int *equalp)
{
	WT_CURSOR *a, *b;
	WT_SESSION_IMPL *session;
	int cmp;

	a = (WT_CURSOR *)a_arg;
	b = (WT_CURSOR *)b_arg;
	session = (WT_SESSION_IMPL *)a->session;

	/* Confirm both cursors reference the same object. */
	if (a_arg->btree != b_arg->btree)
		WT_RET_MSG(
		    session, EINVAL, "Cursors must reference the same object");

	/*
	 * The reason for an equals method is because we can avoid doing
	 * a full key comparison in some cases. If both cursors point into the
	 * tree, take the fast path, otherwise fall back to the slower compare
	 * method; in both cases, return 1 if the cursors are equal, 0 if they
	 * are not.
	 */
	if (F_ISSET(a, WT_CURSTD_KEY_INT) && F_ISSET(b, WT_CURSTD_KEY_INT))
		*equalp = __cursor_equals(a_arg, b_arg);
	else {
		WT_RET(__wt_btcur_compare(a_arg, b_arg, &cmp));
		*equalp = (cmp == 0) ? 1 : 0;
	}
	return (0);
}

/*
 * __cursor_truncate --
 *	Discard a cursor range from row-store or variable-width column-store
 * tree.
 */
static int
__cursor_truncate(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop,
    int (*rmfunc)(WT_SESSION_IMPL *, WT_CURSOR_BTREE *, int))
{
	WT_DECL_RET;

	/*
	 * First, call the standard cursor remove method to do a full search and
	 * re-position the cursor because we don't have a saved copy of the
	 * page's write generation information, which we need to remove records.
	 * Once that's done, we can delete records without a full search, unless
	 * we encounter a restart error because the page was modified by some
	 * other thread of control; in that case, repeat the full search to
	 * refresh the page's modification information.
	 *
	 * If this is a row-store, we delete leaf pages having no overflow items
	 * without reading them; for that to work, we have to ensure we read the
	 * page referenced by the ending cursor, since we may be deleting only a
	 * partial page at the end of the truncation.  Our caller already fully
	 * instantiated the end cursor, so we know that page is pinned in memory
	 * and we can proceed without concern.
	 */
	if (start == NULL) {
		do {
			WT_RET(__wt_btcur_remove(stop));
			for (;;) {
				if ((ret = __wt_btcur_prev(stop, 1)) != 0)
					break;
				stop->compare = 0;	/* Exact match */
				if ((ret = rmfunc(session, stop, 1)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else {
		do {
			WT_RET(__wt_btcur_remove(start));
			/*
			 * Reset ret each time through so that we don't loop
			 * forever in the cursor equals case.
			 */
			for (ret = 0;;) {
				if (stop != NULL &&
				    __cursor_equals(start, stop))
					break;
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				if ((ret = rmfunc(session, start, 1)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	}

	WT_RET_NOTFOUND_OK(ret);
	return (0);
}

/*
 * __cursor_truncate_fix --
 *	Discard a cursor range from fixed-width column-store tree.
 */
static int
__cursor_truncate_fix(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop,
    int (*rmfunc)(WT_SESSION_IMPL *, WT_CURSOR_BTREE *, int))
{
	WT_DECL_RET;
	uint8_t *value;

	/*
	 * Handle fixed-length column-store objects separately: for row-store
	 * and variable-length column-store objects we have "deleted" values
	 * and so returned objects actually exist: fixed-length column-store
	 * objects are filled-in if they don't exist, that is, if you create
	 * record 37, records 1-36 magically appear.  Those records can't be
	 * deleted, which means we have to ignore already "deleted" records.
	 *
	 * First, call the standard cursor remove method to do a full search and
	 * re-position the cursor because we don't have a saved copy of the
	 * page's write generation information, which we need to remove records.
	 * Once that's done, we can delete records without a full search, unless
	 * we encounter a restart error because the page was modified by some
	 * other thread of control; in that case, repeat the full search to
	 * refresh the page's modification information.
	 */
	if (start == NULL) {
		do {
			WT_RET(__wt_btcur_remove(stop));
			for (;;) {
				if ((ret = __wt_btcur_prev(stop, 1)) != 0)
					break;
				stop->compare = 0;	/* Exact match */
				value = (uint8_t *)stop->iface.value.data;
				if (*value != 0 &&
				    (ret = rmfunc(session, stop, 1)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else {
		do {
			WT_RET(__wt_btcur_remove(start));
			/*
			 * Reset ret each time through so that we don't loop
			 * forever in the cursor equals case.
			 */
			for (ret = 0;;) {
				if (stop != NULL &&
				    __cursor_equals(start, stop))
					break;
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				value = (uint8_t *)start->iface.value.data;
				if (*value != 0 &&
				    (ret = rmfunc(session, start, 1)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	}

	WT_RET_NOTFOUND_OK(ret);
	return (0);
}

/*
 * __wt_btcur_range_truncate --
 *	Discard a cursor range from the tree.
 */
int
__wt_btcur_range_truncate(WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop)
{
	WT_BTREE *btree;
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (start != NULL) ? start : stop;
	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = cbt->btree;

	/*
	 * For recovery, we log the start and stop keys for a truncate
	 * operation, not the individual records removed.  On the other hand,
	 * for rollback we need to keep track of all the in-memory operations.
	 *
	 * We deal with this here by logging the truncate range first, then (in
	 * the logging code) disabling writing of the in-memory remove records
	 * to disk.
	 */
	if (FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED))
		WT_RET(__wt_txn_truncate_log(session, start, stop));

	switch (btree->type) {
	case BTREE_COL_FIX:
		WT_ERR(__cursor_truncate_fix(
		    session, start, stop, __cursor_col_modify));
		break;
	case BTREE_COL_VAR:
		WT_ERR(__cursor_truncate(
		    session, start, stop, __cursor_col_modify));
		break;
	case BTREE_ROW:
		/*
		 * The underlying cursor comparison routine requires cursors be
		 * fully instantiated when truncating row-store objects because
		 * it's comparing page and/or skiplist positions, not keys. (Key
		 * comparison would work, it's only that a key comparison would
		 * be relatively expensive.  Column-store objects have record
		 * number keys, so the key comparison is cheap.)  Cursors may
		 * have only had their keys set, so we must ensure the cursors
		 * are positioned in the tree.
		 */
		if (start != NULL)
			WT_ERR(__wt_btcur_search(start));
		if (stop != NULL)
			WT_ERR(__wt_btcur_search(stop));
		WT_ERR(__cursor_truncate(
		    session, start, stop, __cursor_row_modify));
		break;
	}

err:	if (FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED))
		WT_TRET(__wt_txn_truncate_end(session));
	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	ret = __curfile_leave(cbt);
	__wt_buf_free(session, &cbt->search_key);
	__wt_buf_free(session, &cbt->tmp);

	return (ret);
}
