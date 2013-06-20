/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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

	btree = S2BT(session);

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
	if (ins != NULL) {
		if ((upd = __wt_txn_read(session, ins->upd)) == NULL)
			return (1);
		return (WT_UPDATE_DELETED_ISSET(upd) ? 1 : 0);
	}

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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_CSTAT_INCR(session, cursor_reset);
	WT_DSTAT_INCR(session, cursor_reset);

	ret = __cursor_leave(cbt);
	__cursor_search_clear(cbt);
	__cursor_position_clear(cbt);

	return (ret);
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

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_CSTAT_INCR(session, cursor_search);
	WT_DSTAT_INCR(session, cursor_search);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

retry:	WT_RET(__cursor_func_init(cbt, 1));

	WT_ERR(btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) :
	    __wt_col_search(session, cbt, 0));
	if (cbt->compare != 0 || __cursor_invalid(cbt)) {
		/*
		 * Creating a record past the end of the tree in a fixed-length
		 * column-store implicitly fills the gap with empty records.
		 */
		if (__cursor_fix_implicit(btree, cbt)) {
			cbt->recno = cursor->recno;
			cbt->v = 0;
			cursor->value.data = &cbt->v;
			cursor->value.size = 1;
		} else
			ret = WT_NOTFOUND;
	} else
		ret = __wt_kv_return(session, cbt);

err:	if (ret == WT_RESTART)
		goto retry;
	WT_TRET(__cursor_func_resolve(cbt, ret));
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
	WT_SESSION_IMPL *session;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_CSTAT_INCR(session, cursor_search_near);
	WT_DSTAT_INCR(session, cursor_search_near);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

retry:	WT_RET(__cursor_func_init(cbt, 1));

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
		cbt->recno = cursor->recno;
		cbt->v = 0;
		cursor->value.data = &cbt->v;
		cursor->value.size = 1;
		*exact = 0;
	} else if (!__cursor_invalid(cbt)) {
		*exact = cbt->compare;
		ret = __wt_kv_return(session, cbt);
	} else if ((ret = __wt_btcur_next(cbt, 0)) != WT_NOTFOUND)
		*exact = 1;
	else {
		WT_ERR(btree->type == BTREE_ROW ?
		    __wt_row_search(session, cbt, 0) :
		    __wt_col_search(session, cbt, 0));
		if (!__cursor_invalid(cbt)) {
			*exact = cbt->compare;
			ret = __wt_kv_return(session, cbt);
		} else if ((ret = __wt_btcur_prev(cbt, 0)) != WT_NOTFOUND)
			*exact = -1;
	}

err:	if (ret == WT_RESTART)
		goto retry;
	WT_TRET(__cursor_func_resolve(cbt, ret));
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

	WT_CSTAT_INCR(session, cursor_insert);
	WT_DSTAT_INCR(session, cursor_insert);
	WT_DSTAT_INCRV(session,
	    cursor_insert_bytes, cursor->key.size + cursor->value.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

	/*
	 * The tree is no longer empty: eviction should pay attention to it,
	 * and it's no longer possible to bulk-load into it.
	 */
	btree->bulk_load_ok = 0;

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

		WT_ERR(__wt_col_search(session, cbt, 1));

		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = 0;

		/*
		 * If not overwriting, fail if the key exists.  Creating a
		 * record past the end of the tree in a fixed-length
		 * column-store implicitly fills the gap with empty records.
		 * Fail in that case, the record exists.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    ((cbt->compare == 0 && !__cursor_invalid(cbt)) ||
		    (cbt->compare != 0 && __cursor_fix_implicit(btree, cbt))))
			WT_ERR(WT_DUPLICATE_KEY);

		WT_ERR(__wt_col_modify(session, cbt, 3));
		if (F_ISSET(cursor, WT_CURSTD_APPEND))
			cbt->iface.recno = cbt->recno;
		break;
	case BTREE_ROW:
		WT_ERR(__wt_row_search(session, cbt, 1));
		/*
		 * If not overwriting, fail if the key exists, else insert the
		 * key/value pair.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    cbt->compare == 0 && !__cursor_invalid(cbt))
			WT_ERR(WT_DUPLICATE_KEY);

		ret = __wt_row_modify(session, cbt, 0);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;
	WT_TRET(__cursor_func_resolve(cbt, ret));
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

	WT_CSTAT_INCR(session, cursor_remove);
	WT_DSTAT_INCR(session, cursor_remove);
	WT_DSTAT_INCRV(session, cursor_remove_bytes, cursor->key.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));

retry:	WT_RET(__cursor_func_init(cbt, 1));

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__wt_col_search(session, cbt, 1));

		/* Remove the record if it exists. */
		if (cbt->compare != 0 || __cursor_invalid(cbt)) {
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
			ret = __wt_col_modify(session, cbt, 2);
		break;
	case BTREE_ROW:
		/* Remove the record if it exists. */
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_invalid(cbt))
			WT_ERR(WT_NOTFOUND);

		ret = __wt_row_modify(session, cbt, 1);
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
	WT_TRET(__cursor_func_resolve(cbt, ret));
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

	WT_CSTAT_INCR(session, cursor_update);
	WT_DSTAT_INCR(session, cursor_update);
	WT_DSTAT_INCRV(session, cursor_update_bytes, cursor->value.size);

	if (btree->type == BTREE_ROW)
		WT_RET(__cursor_size_chk(session, &cursor->key));
	WT_RET(__cursor_size_chk(session, &cursor->value));

retry:	WT_RET(__cursor_func_init(cbt, 1));

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
		 * If not overwriting, fail if the key doesn't exist.  Update
		 * the record if it exists.  Creating a record past the end of
		 * the tree in a fixed-length column-store implicitly fills the
		 * gap with empty records.  Update the record in that case, the
		 * record exists.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    (cbt->compare != 0 || __cursor_invalid(cbt)) &&
		    !__cursor_fix_implicit(btree, cbt))
			WT_ERR(WT_NOTFOUND);
		ret = __wt_col_modify(session, cbt, 3);
		break;
	case BTREE_ROW:
		WT_ERR(__wt_row_search(session, cbt, 1));
		/*
		 * If not overwriting, fail if the key does not exist.
		 */
		if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
		    (cbt->compare != 0 || __cursor_invalid(cbt)))
			WT_ERR(WT_NOTFOUND);
		ret = __wt_row_modify(session, cbt, 0);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	if (ret == WT_RESTART)
		goto retry;
	WT_TRET(__cursor_func_resolve(cbt, ret));
	return (ret);
}

/*
 * __wt_btcur_compare --
 *	Return a comparison between two cursors.
 */
int
__wt_btcur_compare(WT_CURSOR_BTREE *a_arg, WT_CURSOR_BTREE *b_arg, int *cmpp)
{
	WT_BTREE *btree;
	WT_CURSOR *a, *b;
	WT_SESSION_IMPL *session;

	a = (WT_CURSOR *)a_arg;
	b = (WT_CURSOR *)b_arg;
	btree = a_arg->btree;
	session = (WT_SESSION_IMPL *)a->session;

	switch (btree->type) {
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
		WT_RET(WT_BTREE_CMP(
		    session, btree, &a->key, &b->key, *cmpp));
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * __cursor_equals --
 *	Return if two cursors reference the same row.
 */
static int
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
		if (a->page != b->page)
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
				if ((ret = rmfunc(session, stop, 2)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else if (stop == NULL) {
		do {
			WT_RET(__wt_btcur_remove(start));
			for (;;) {
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				if ((ret = rmfunc(session, start, 2)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else {

		do {
			WT_RET(__wt_btcur_remove(start));
			for (;;) {
				if (__cursor_equals(start, stop))
					break;
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				if ((ret = rmfunc(session, start, 2)) != 0)
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
				    (ret = rmfunc(session, stop, 2)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else if (stop == NULL) {
		value = (uint8_t *)&start->iface.value;
		do {
			WT_RET(__wt_btcur_remove(start));
			for (;;) {
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				value = (uint8_t *)start->iface.value.data;
				if (*value != 0 &&
				    (ret = rmfunc(session, start, 2)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	} else {
		value = (uint8_t *)&start->iface.value;
		do {
			WT_RET(__wt_btcur_remove(start));
			for (;;) {
				if (__cursor_equals(start, stop))
					break;
				if ((ret = __wt_btcur_next(start, 1)) != 0)
					break;
				start->compare = 0;	/* Exact match */
				value = (uint8_t *)start->iface.value.data;
				if (*value != 0 &&
				    (ret = rmfunc(session, start, 2)) != 0)
					break;
			}
		} while (ret == WT_RESTART);
	}

	WT_RET_NOTFOUND_OK(ret);
	return (0);
}

/*
 * __wt_btcur_truncate --
 *	Discard a cursor range from the tree.
 */
int
__wt_btcur_truncate(WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	if (start == NULL) {
		session = (WT_SESSION_IMPL *)stop->iface.session;
		btree = stop->btree;
	} else {
		session = (WT_SESSION_IMPL *)start->iface.session;
		btree = start->btree;
	}

	switch (btree->type) {
	case BTREE_COL_FIX:
		WT_RET(__cursor_truncate_fix(
		    session, start, stop, __wt_col_modify));
		break;
	case BTREE_COL_VAR:
		WT_RET(__cursor_truncate(
		    session, start, stop, __wt_col_modify));
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
			WT_RET(__wt_btcur_search(start));
		if (stop != NULL)
			WT_RET(__wt_btcur_search(stop));
		WT_RET(__cursor_truncate(
		    session, start, stop, __wt_row_modify));
		break;
	}
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

	ret = __cursor_leave(cbt);
	__wt_buf_free(session, &cbt->tmp);

	return (ret);
}
